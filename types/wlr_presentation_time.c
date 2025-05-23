#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/backend.h>
#include "presentation-time-protocol.h"
#include "util/signal.h"

#define PRESENTATION_VERSION 1

static void feedback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void feedback_resource_send_presented(
		struct wl_resource *feedback_resource,
		struct wlr_presentation_event *event) {
	struct wl_client *client = wl_resource_get_client(feedback_resource);
	struct wl_resource *output_resource;
	wl_resource_for_each(output_resource, &event->output->resources) {
		if (wl_resource_get_client(output_resource) == client) {
			wp_presentation_feedback_send_sync_output(feedback_resource,
				output_resource);
		}
	}

	uint32_t tv_sec_hi = event->tv_sec >> 32;
	uint32_t tv_sec_lo = event->tv_sec & 0xFFFFFFFF;
	uint32_t seq_hi = event->seq >> 32;
	uint32_t seq_lo = event->seq & 0xFFFFFFFF;
	wp_presentation_feedback_send_presented(feedback_resource,
		tv_sec_hi, tv_sec_lo, event->tv_nsec, event->refresh,
		seq_hi, seq_lo, event->flags);

	wl_resource_destroy(feedback_resource);
}

static void feedback_resource_send_discarded(
		struct wl_resource *feedback_resource) {
	wp_presentation_feedback_send_discarded(feedback_resource);
	wl_resource_destroy(feedback_resource);
}

static void feedback_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct wlr_presentation_feedback *feedback =
		wl_container_of(listener, feedback, surface_commit);

	if (feedback->committed) {
		if (!feedback->sampled) {
			// The content update has been superseded
			wlr_presentation_feedback_destroy(feedback);
		}
	} else {
		feedback->committed = true;
	}
}

static void feedback_unset_surface(struct wlr_presentation_feedback *feedback) {
	if (feedback->surface == NULL) {
		return;
	}

	feedback->surface = NULL;
	wl_list_remove(&feedback->surface_commit.link);
	wl_list_remove(&feedback->surface_destroy.link);
}

static void feedback_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_presentation_feedback *feedback =
		wl_container_of(listener, feedback, surface_destroy);
	if (feedback->sampled) {
		// The compositor might have a handle on this feedback
		feedback_unset_surface(feedback);
	} else {
		wlr_presentation_feedback_destroy(feedback);
	}
}

static const struct wp_presentation_interface presentation_impl;

static struct wlr_presentation *presentation_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_presentation_interface,
		&presentation_impl));
	return wl_resource_get_user_data(resource);
}

static void presentation_handle_feedback(struct wl_client *client,
		struct wl_resource *presentation_resource,
		struct wl_resource *surface_resource, uint32_t id) {
	struct wlr_presentation *presentation =
		presentation_from_resource(presentation_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	bool found = false;
	struct wlr_presentation_feedback *feedback;
	wl_list_for_each(feedback, &presentation->feedbacks, link) {
		if (feedback->surface == surface && !feedback->committed) {
			found = true;
			break;
		}
	}
	if (!found) {
		feedback = calloc(1, sizeof(struct wlr_presentation_feedback));
		if (feedback == NULL) {
			wl_client_post_no_memory(client);
			return;
		}

		feedback->surface = surface;
		wl_list_init(&feedback->resources);

		feedback->surface_commit.notify = feedback_handle_surface_commit;
		wl_signal_add(&surface->events.commit, &feedback->surface_commit);

		feedback->surface_destroy.notify = feedback_handle_surface_destroy;
		wl_signal_add(&surface->events.destroy, &feedback->surface_destroy);

		wl_list_insert(&presentation->feedbacks, &feedback->link);
	}

	uint32_t version = wl_resource_get_version(presentation_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_presentation_feedback_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, NULL, feedback,
		feedback_handle_resource_destroy);

	wl_list_insert(&feedback->resources, wl_resource_get_link(resource));
}

static void presentation_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_presentation_interface presentation_impl = {
	.feedback = presentation_handle_feedback,
	.destroy = presentation_handle_destroy,
};

static void presentation_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_presentation *presentation = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_presentation_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &presentation_impl, presentation,
		NULL);

	wp_presentation_send_clock_id(resource, (uint32_t)presentation->clock);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_presentation *presentation =
		wl_container_of(listener, presentation, display_destroy);
	wlr_signal_emit_safe(&presentation->events.destroy, presentation);
	wl_list_remove(&presentation->display_destroy.link);
	wl_global_destroy(presentation->global);
	free(presentation);
}

struct wlr_presentation *wlr_presentation_create(struct wl_display *display,
		struct wlr_backend *backend) {
	struct wlr_presentation *presentation =
		calloc(1, sizeof(struct wlr_presentation));
	if (presentation == NULL) {
		return NULL;
	}

	presentation->global = wl_global_create(display, &wp_presentation_interface,
		PRESENTATION_VERSION, presentation, presentation_bind);
	if (presentation->global == NULL) {
		free(presentation);
		return NULL;
	}

	presentation->clock = wlr_backend_get_presentation_clock(backend);

	wl_list_init(&presentation->feedbacks);
	wl_signal_init(&presentation->events.destroy);

	presentation->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &presentation->display_destroy);

	return presentation;
}

void wlr_presentation_feedback_send_presented(
		struct wlr_presentation_feedback *feedback,
		struct wlr_presentation_event *event) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &feedback->resources) {
		feedback_resource_send_presented(resource, event);
	}

	feedback->presented = true;
}

struct wlr_presentation_feedback *wlr_presentation_surface_sampled(
		struct wlr_presentation *presentation, struct wlr_surface *surface) {
	// TODO: maybe use a hashtable to optimize this function
	struct wlr_presentation_feedback *feedback, *feedback_tmp;
	wl_list_for_each_safe(feedback, feedback_tmp,
			&presentation->feedbacks, link) {
		if (feedback->surface == surface && feedback->committed &&
				!feedback->sampled) {
			feedback->sampled = true;
			return feedback;
		}
	}
	return NULL;
}

static void feedback_unset_output(struct wlr_presentation_feedback *feedback);

void wlr_presentation_feedback_destroy(
		struct wlr_presentation_feedback *feedback) {
	if (feedback == NULL) {
		return;
	}

	if (!feedback->presented) {
		struct wl_resource *resource, *tmp;
		wl_resource_for_each_safe(resource, tmp, &feedback->resources) {
			feedback_resource_send_discarded(resource);
		}
	}
	assert(wl_list_empty(&feedback->resources));

	feedback_unset_surface(feedback);
	feedback_unset_output(feedback);
	wl_list_remove(&feedback->link);
	free(feedback);
}

void wlr_presentation_event_from_output(struct wlr_presentation_event *event,
		const struct wlr_output_event_present *output_event) {
	memset(event, 0, sizeof(*event));
	event->output = output_event->output;
	event->tv_sec = (uint64_t)output_event->when->tv_sec;
	event->tv_nsec = (uint32_t)output_event->when->tv_nsec;
	event->refresh = (uint32_t)output_event->refresh;
	event->seq = (uint64_t)output_event->seq;
	event->flags = output_event->flags;
}

static void feedback_unset_output(struct wlr_presentation_feedback *feedback) {
	if (feedback->output == NULL) {
		return;
	}

	feedback->output = NULL;
	wl_list_remove(&feedback->output_commit.link);
	wl_list_remove(&feedback->output_present.link);
	wl_list_remove(&feedback->output_destroy.link);
}

static void feedback_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct wlr_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_commit);
	if (feedback->output_committed) {
		return;
	}
	feedback->output_committed = true;
	feedback->output_commit_seq = feedback->output->commit_seq;
}

static void feedback_handle_output_present(struct wl_listener *listener,
		void *data) {
	struct wlr_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_present);
	struct wlr_output_event_present *output_event = data;

	if (!feedback->output_committed ||
			output_event->commit_seq != feedback->output_commit_seq) {
		return;
	}

	if (output_event->presented) {
		struct wlr_presentation_event event = {0};
		wlr_presentation_event_from_output(&event, output_event);
		wlr_presentation_feedback_send_presented(feedback, &event);
	}
	wlr_presentation_feedback_destroy(feedback);
}

static void feedback_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_destroy);
	wlr_presentation_feedback_destroy(feedback);
}

void wlr_presentation_surface_sampled_on_output(
		struct wlr_presentation *presentation, struct wlr_surface *surface,
		struct wlr_output *output) {
	struct wlr_presentation_feedback *feedback =
		wlr_presentation_surface_sampled(presentation, surface);
	if (feedback == NULL) {
		return;
	}

	assert(feedback->output == NULL);
	feedback->output = output;

	feedback->output_commit.notify = feedback_handle_output_commit;
	wl_signal_add(&output->events.commit, &feedback->output_commit);
	feedback->output_present.notify = feedback_handle_output_present;
	wl_signal_add(&output->events.present, &feedback->output_present);
	feedback->output_destroy.notify = feedback_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &feedback->output_destroy);
}
