/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>

#include "cow/module.h"
#include "cow/module_status.h"
#include "cow/attrs.h"

static void stream_connect(struct cow_module_status_stream *);

static void
stream_schedule_retry(struct cow_module_status_stream *stream)
{
	if (stream->stopping || event_pending(stream->retry_event,
		EV_TIMEOUT, NULL))
		return;
	struct timeval delay = { .tv_sec = COW_MODULE_STATUS_RETRY_DELAY };
	evtimer_add(stream->retry_event, &delay);
}

static void
stream_disconnect(struct cow_module_status_stream *stream)
{
	if (stream->bev != NULL) {
		bufferevent_free(stream->bev);
		stream->bev = NULL;
	}
	stream_schedule_retry(stream);
}

static void
stream_read_cb(struct bufferevent *bev, void *data)
{
	struct cow_module_status_stream *stream = data;
	struct evbuffer *input = bufferevent_get_input(bev);

	for (;;) {
		struct evbuffer_ptr end = evbuffer_search_eol(input, NULL, NULL,
		    EVBUFFER_EOL_LF);
		if (end.pos < 0) {
			if (evbuffer_get_length(input) > stream->max_line)
				stream_disconnect(stream);
			return;
		}
		if ((size_t)end.pos > stream->max_line) {
			stream_disconnect(stream);
			return;
		}
		size_t len = 0;
		char *line = evbuffer_readln(input, &len, EVBUFFER_EOL_LF);
		if (line == NULL)
			return;
		stream->callback(line, stream->data);
		free(line);
		if (stream->bev == NULL)
			return;
	}
}

static void
stream_event_cb(__unused struct bufferevent *bev, short events, void *data)
{
	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		stream_disconnect(data);
}

static void
stream_connect(struct cow_module_status_stream *stream)
{
	if (stream->stopping || stream->bev != NULL)
		return;
	int fd = cow_module_status_connect_quiet(stream->program);
	if (fd < 0) {
		stream_schedule_retry(stream);
		return;
	}
	evutil_make_socket_nonblocking(fd);
	stream->bev = bufferevent_socket_new(stream->base, fd,
	    BEV_OPT_CLOSE_ON_FREE);
	if (stream->bev == NULL) {
		close(fd);
		stream_schedule_retry(stream);
		return;
	}
	bufferevent_setcb(stream->bev, stream_read_cb, NULL, stream_event_cb,
	    stream);
	bufferevent_enable(stream->bev, EV_READ);
}

static void
stream_retry_cb(__unused evutil_socket_t fd, __unused short events, void *data)
{
	stream_connect(data);
}

bool
cow_module_status_stream_init(struct cow_module_status_stream *stream,
    struct event_base *base, const char *program, cow_module_status_fn callback,
    void *data)
{
	if (stream == NULL || base == NULL || program == NULL || callback == NULL)
		return false;
	*stream = (struct cow_module_status_stream) {
		.base = base,
		.program = strdup(program),
		.callback = callback,
		.data = data,
		.max_line = COW_MODULE_STATUS_MAX_LINE,
	};
	if (stream->program == NULL)
		return false;
	stream->retry_event = evtimer_new(base, stream_retry_cb, stream);
	if (stream->retry_event == NULL) {
		free(stream->program);
		stream->program = NULL;
		return false;
	}
	stream_connect(stream);
	return true;
}

void
cow_module_status_stream_fini(struct cow_module_status_stream *stream)
{
	if (stream == NULL)
		return;
	stream->stopping = true;
	if (stream->retry_event != NULL) {
		event_free(stream->retry_event);
		stream->retry_event = NULL;
	}
	if (stream->bev != NULL) {
		bufferevent_free(stream->bev);
		stream->bev = NULL;
	}
	free(stream->program);
	stream->program = NULL;
}
