#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/timeline.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

/* Simple compositor with explicit synchronization support. Input is
 * unimplemented.
 *
 * New surfaces are stacked on top of the existing ones as they appear. */

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_allocator *allocator;
	struct wlr_renderer *renderer;
	struct wlr_linux_drm_syncobj_manager_v1 *drm_syncobj_manager_v1;

	struct wl_list outputs;
	struct wl_list surfaces;

	struct wl_listener new_output;
	struct wl_listener new_surface;
};

struct surface {
	struct wlr_surface *wlr;
	struct wl_list link;

	struct wl_listener commit;
	struct wl_listener destroy;

	struct output *last_output;
	uint64_t last_output_point;
};

struct output {
	struct wl_list link;
	struct wlr_output *wlr;

	struct wlr_render_timeline *in_timeline, *out_timeline;

	struct wl_listener frame;
};

static struct server server = {0};

static void output_handle_frame(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_renderer *renderer = server.renderer;

	wlr_output_configure_primary_swapchain(output->wlr, NULL, &output->wlr->swapchain);

	uint64_t output_point = output->wlr->commit_seq;
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->wlr->swapchain, NULL);
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, &(struct wlr_buffer_pass_options){
		.signal_timeline = output->in_timeline,
		.signal_point = output_point,
	});

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = output->wlr->width, .height = output->wlr->height },
		.color = { 0.25, 0.25, 0.25, 1 },
	});

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int pos = 0;
	struct surface *surface;
	wl_list_for_each(surface, &server.surfaces, link) {
		pos += 50;

		struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr);
		if (texture == NULL) {
			continue;
		}

		struct wlr_render_timeline *wait_timeline = NULL;
		uint64_t wait_point = 0;
		struct wlr_linux_drm_syncobj_surface_v1_state *sync_v2_state =
			wlr_linux_drm_syncobj_v1_get_surface_state(
			server.drm_syncobj_manager_v1, surface->wlr);
		if (sync_v2_state != NULL) {
			wait_timeline = sync_v2_state->acquire_timeline;
			wait_point = sync_v2_state->acquire_point;
		} else {
			wlr_log(WLR_ERROR, "Client doesn't support linux-drm-syncobj-v1");
			continue;
		}

		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.dst_box = { .x = pos, .y = pos },
			.wait_timeline = wait_timeline,
			.wait_point = wait_point,
		});

		wlr_surface_send_frame_done(surface->wlr, &now);

		surface->last_output = output;
		surface->last_output_point = output_point;
	}

	wlr_render_pass_submit(pass);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_buffer(&state, buffer);
	wlr_buffer_unlock(buffer);
	wlr_output_state_set_wait_timeline(&state, output->in_timeline, output_point);
	wlr_output_state_set_signal_timeline(&state, output->out_timeline, output_point);
	wlr_output_commit_state(output->wlr, &state);
	wlr_output_state_finish(&state);

	wl_list_for_each(surface, &server.surfaces, link) {
		struct wlr_linux_drm_syncobj_surface_v1_state *sync_v2_state =
			wlr_linux_drm_syncobj_v1_get_surface_state(
			server.drm_syncobj_manager_v1, surface->wlr);
		if (sync_v2_state != NULL) {
			if (!wlr_render_timeline_transfer(sync_v2_state->release_timeline,
					sync_v2_state->release_point, output->out_timeline,
					output_point)) {
				wlr_log(WLR_ERROR, "Failed to transfer surface release timeline");
			}
		}
	}
}

static void server_handle_new_output(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output = data;

	if (!wlr_output->timeline) {
		wlr_log(WLR_ERROR, "Output doesn't support timelines");
		return;
	}

	wlr_output_init_render(wlr_output, server.allocator, server.renderer);

	int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
	struct wlr_render_timeline *in_timeline = wlr_render_timeline_create(drm_fd);
	struct wlr_render_timeline *out_timeline = wlr_render_timeline_create(drm_fd);
	if (in_timeline == NULL || out_timeline == NULL) {
		return;
	}

	struct output *output = calloc(1, sizeof(*output));
	output->wlr = wlr_output;
	output->in_timeline = in_timeline;
	output->out_timeline = out_timeline;
	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server.outputs, &output->link);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wlr_output_create_global(wlr_output, server.display);
}

static void surface_handle_commit(struct wl_listener *listener, void *data) {
	struct surface *surface = wl_container_of(listener, surface, commit);
	if (!(surface->wlr->current.committed & WLR_SURFACE_STATE_BUFFER)) {
		return;
	}
	struct wlr_linux_drm_syncobj_surface_v1_state *sync_v2_state =
		wlr_linux_drm_syncobj_v1_get_surface_state(
		server.drm_syncobj_manager_v1, surface->wlr);
	if (sync_v2_state == NULL) {
		return;
	}
	// TODO: support multiple outputs
	struct output *output = surface->last_output;
	uint64_t output_point = surface->last_output_point;
	if (output == NULL) {
		return; // TODO: signal immediately
	}
	if (!wlr_render_timeline_transfer(sync_v2_state->release_timeline,
			sync_v2_state->release_point, output->out_timeline,
			output_point)) {
		wlr_log(WLR_ERROR, "Failed to transfer surface release timeline");
	}
}

static void surface_handle_destroy(struct wl_listener *listener, void *data) {
	struct surface *surface = wl_container_of(listener, surface, destroy);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->commit.link);
	wl_list_remove(&surface->link);
	free(surface);
}

static void server_handle_new_surface(struct wl_listener *listener,
		void *data) {
	struct wlr_surface *wlr_surface = data;

	struct surface *surface = calloc(1, sizeof(*surface));
	surface->wlr = wlr_surface;
	surface->commit.notify = surface_handle_commit;
	wl_signal_add(&wlr_surface->events.commit, &surface->commit);
	surface->destroy.notify = surface_handle_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->destroy);

	wl_list_insert(&server.surfaces, &surface->link);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);

	const char *startup_cmd = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("usage: %s [-s startup-command]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind < argc) {
		printf("usage: %s [-s startup-command]\n", argv[0]);
		return EXIT_FAILURE;
	}

	server.display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(loop, NULL);
	server.renderer = wlr_renderer_autocreate(server.backend);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	wlr_renderer_init_wl_display(server.renderer, server.display);

	if (!server.renderer->features.timeline) {
		wlr_log(WLR_ERROR, "Renderer doesn't support timelines");
		return EXIT_FAILURE;
	}

	struct wlr_compositor *compositor = wlr_compositor_create(server.display, 5, server.renderer);

	wlr_xdg_shell_create(server.display, 2);

	int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
	server.drm_syncobj_manager_v1 =
		wlr_linux_drm_syncobj_manager_v1_create(server.display, 1, drm_fd);

	wl_list_init(&server.outputs);
	wl_list_init(&server.surfaces);

	server.new_output.notify = server_handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.new_surface.notify = server_handle_new_surface;
	wl_signal_add(&compositor->events.new_surface, &server.new_surface);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		wl_display_destroy(server.display);
		return EXIT_FAILURE;
	}

	if (!wlr_backend_start(server.backend)) {
		wl_display_destroy(server.display);
		return EXIT_FAILURE;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd != NULL) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return EXIT_SUCCESS;
}
