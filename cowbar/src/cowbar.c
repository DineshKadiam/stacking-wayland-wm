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
/*
 * cowbar -- status relay for the cow window manager.
 *
 * Connects to cow's status socket and relays JSON state snapshots
 * to stdout, one line per update.  Optionally filters by output
 * name and/or extracts a specific field.
 *
 * Usage:
 *   cowbar                            # relay full snapshots
 *   cowbar -o DP-1                    # filter for output DP-1
 *   cowbar -f desks -o DP-1           # extract desks field for DP-1
 *   cowbar -f focused                 # extract focused window info
 *   cowbar -w desks -o DP-1           # waybar JSON for desks module
 *   cowbar -w title -o DP-1           # waybar JSON for title module
 *
 * Output is one JSON line per state change on stdout.
 * Without -w: no styling -- consumers handle presentation.
 * With -w: pre-formatted JSON ({"text":..., "tooltip":...}) for waybar.
 */

#include <cJSON.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: cowbar [-o output] [-f field] [-w module]\n"
	    "\n"
	    "  -o output   filter for named output\n"
	    "  -f field    extract field: desks, focused, outputs\n"
	    "  -w module   emit waybar JSON for module: desks, title\n"
	    "              (requires -o for desks; -o filters title to one bar)\n"
	    "\n"
	    "With no flags, relays full state snapshots.\n");
}

/*
 * Find the output object matching the given name.
 * Returns NULL if not found or name is NULL (returns first).
 */
static cJSON *
find_output(cJSON *state, const char *name)
{
	cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
	if (!cJSON_IsArray(outputs))
		return NULL;

	cJSON *out;
	cJSON_ArrayForEach(out, outputs)
	{
		if (name == NULL)
			return out;
		cJSON *n = cJSON_GetObjectItem(out, "name");
		if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
			return out;
	}
	return NULL;
}

static void
emit(cJSON *obj)
{
	char *s = cJSON_PrintUnformatted(obj);
	if (s != NULL) {
		puts(s);
		fflush(stdout);
		cJSON_free(s);
	}
}

/* Append printf-style to a dynamic buffer.  *buf is realloc'd as needed.
 * On success, *len is the new length (excluding NUL). */
static void
buf_appendf(char **buf, size_t *cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	for (;;) {
		size_t avail = *cap - *len;
		va_start(ap, fmt);
		int n = vsnprintf(*buf + *len, avail, fmt, ap);
		va_end(ap);
		if (n < 0)
			return;
		if ((size_t)n < avail) {
			*len += n;
			return;
		}
		size_t need = *len + (size_t)n + 1;
		size_t ncap = *cap > 256 ? *cap : 256;
		while (ncap < need)
			ncap *= 2;
		char *nb = realloc(*buf, ncap);
		if (nb == NULL)
			return;
		*buf = nb;
		*cap = ncap;
	}
}

/*
 * Emit a single {"text":..., "tooltip":...} JSON object.  Both strings are
 * pre-rendered so the consumer (waybar) doesn't need to format anything.
 */
static void
emit_waybar(const char *text, const char *tooltip)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "text", text != NULL ? text : "");
	cJSON_AddStringToObject(obj, "tooltip", tooltip != NULL ? tooltip : "");
	emit(obj);
	cJSON_Delete(obj);
}

/* Render the waybar 'desks' module text for the given output.  The
 * output object is the per-output entry from cow's state. */
static void
waybar_desks(const cJSON *state, const cJSON *out_obj)
{
	cJSON *desks_arr = cJSON_GetObjectItem(out_obj, "desks");
	if (!cJSON_IsArray(desks_arr)) {
		emit_waybar("", "");
		return;
	}

	char  *text = NULL;
	size_t cap  = 0;
	size_t len  = 0;

	bool   first_span  = true;
	int    active_wins = 0;
	cJSON *desk;
	cJSON_ArrayForEach(desk, desks_arr)
	{
		bool active = cJSON_IsTrue(cJSON_GetObjectItem(desk, "active"));
		bool collected = cJSON_IsTrue(
		    cJSON_GetObjectItem(desk, "collected"));
		cJSON *nr_j  = cJSON_GetObjectItem(desk, "nr");
		cJSON *win_j = cJSON_GetObjectItem(desk, "windows");
		int    nr = cJSON_IsNumber(nr_j) ? (int)nr_j->valuedouble : 0;
		int wins  = cJSON_IsNumber(win_j) ? (int)win_j->valuedouble : 0;

		if (active)
			active_wins = wins;
		if (!active && !collected && wins == 0)
			continue;

		/* FIXME: needs to be configurable. */
		const char *bg = active ? "#39c488" :
		    collected		? "#cc6600" :
					  "#004C98";

		if (!first_span)
			buf_appendf(&text, &cap, &len,
			    "<span color=\"#808080\">|</span>");
		buf_appendf(&text, &cap, &len,
		    "<span background=\"%s\" color=\"#ffffff\"> %d </span>", bg,
		    nr);
		first_span = false;
	}

	cJSON *page_col = cJSON_GetObjectItem(out_obj, "page_col");
	cJSON *page_row = cJSON_GetObjectItem(out_obj, "page_row");
	cJSON *name	= cJSON_GetObjectItem(out_obj, "name");
	cJSON *cfg	= cJSON_GetObjectItem(state, "desktop_configuration");

	/* FIXME: needs to be configurable. */
	buf_appendf(&text, &cap, &len,
	    "<span color=\"#FF00FF\">|</span>"
	    "<span background=\"#E3A3BE\" color=\"#ffffff\">[%d, %d]</span>"
	    "<span background=\"#D7C72F\" color=\"#ffffff\">"
	    "[Scr:%s][A:%d][L:%s]</span>",
	    page_col && cJSON_IsNumber(page_col) ? (int)page_col->valuedouble :
						   0,
	    page_row && cJSON_IsNumber(page_row) ? (int)page_row->valuedouble :
						   0,
	    name && cJSON_IsString(name) ? name->valuestring : "", active_wins,
	    cfg && cJSON_IsString(cfg) ? cfg->valuestring : "");

	cJSON *cur_desk = cJSON_GetObjectItem(out_obj, "current_desk");
	char   tooltip[64];
	snprintf(tooltip, sizeof(tooltip), "desk %d",
	    cur_desk && cJSON_IsNumber(cur_desk) ? (int)cur_desk->valuedouble :
						   0);

	emit_waybar(text != NULL ? text : "", tooltip);
	free(text);
}

/* Render the waybar 'title' module text. In per-output mode, blanks the
 * title if the focused view is on a different output than 'output_name'.
 * 'output_name' may be NULL (no per-output filtering). */
static void
waybar_title(const cJSON *state, const char *output_name)
{
	cJSON *focused	  = cJSON_GetObjectItem(state, "focused");
	cJSON *cfg	  = cJSON_GetObjectItem(state, "desktop_configuration");
	bool   per_output = cfg != NULL && cJSON_IsString(cfg) &&
	    strcmp(cfg->valuestring, "per-output") == 0;

	if (!cJSON_IsObject(focused)) {
		emit_waybar("", "");
		return;
	}
	if (per_output && output_name != NULL) {
		cJSON *out = cJSON_GetObjectItem(focused, "output");
		if (cJSON_IsString(out) &&
		    strcmp(out->valuestring, output_name) != 0) {
			emit_waybar("", "");
			return;
		}
	}

	cJSON	   *id_j    = cJSON_GetObjectItem(focused, "id");
	cJSON	   *title_j = cJSON_GetObjectItem(focused, "title");
	cJSON	   *app_j   = cJSON_GetObjectItem(focused, "app_id");
	const char *id	    = cJSON_IsString(id_j) ? id_j->valuestring : "";
	const char *title = cJSON_IsString(title_j) ? title_j->valuestring : "";
	const char *app	  = cJSON_IsString(app_j) ? app_j->valuestring : "";

	/* Compute shortest id prefix (>= 8 chars) that's unique among all
	 * known window IDs. */
	cJSON	   *ids_arr  = cJSON_GetObjectItem(state, "window_ids");
	const char *short_id = id;
	char	    short_buf[64];
	if (cJSON_IsArray(ids_arr) && id[0] != '\0') {
		size_t id_len = strlen(id);
		for (size_t plen = 8; plen <= id_len; plen++) {
			bool   unique = true;
			cJSON *o;
			cJSON_ArrayForEach(o, ids_arr)
			{
				if (!cJSON_IsString(o))
					continue;
				const char *cand = o->valuestring;
				if (strcmp(cand, id) == 0)
					continue;
				if (strlen(cand) >= plen &&
				    strncmp(cand, id, plen) == 0) {
					unique = false;
					break;
				}
			}
			if (unique) {
				if (plen >= sizeof(short_buf))
					plen = sizeof(short_buf) - 1;
				memcpy(short_buf, id, plen);
				short_buf[plen] = '\0';
				short_id	= short_buf;
				break;
			}
		}
	}

	char  *text = NULL;
	size_t cap = 0, len = 0;

	/* FIXME: needs to be configurable. */
	buf_appendf(&text, &cap, &len, "        [%s] %s        ", short_id,
	    title);
	emit_waybar(text != NULL ? text : "", app);
	free(text);
}

int
main(int argc, char *argv[])
{
	const char *output_filter = NULL;
	const char *field	  = NULL;
	const char *waybar_mod	  = NULL;
	int	    opt;

	while ((opt = getopt(argc, argv, "o:f:w:h")) != -1) {
		switch (opt) {
		case 'o':
			output_filter = optarg;
			break;
		case 'f':
			field = optarg;
			break;
		case 'w':
			waybar_mod = optarg;
			break;
		case 'h':
		default:
			usage();
			return opt == 'h' ? 0 : 1;
		}
	}

	if (waybar_mod != NULL && strcmp(waybar_mod, "desks") != 0 &&
	    strcmp(waybar_mod, "title") != 0) {
		fprintf(stderr, "cowbar: unknown waybar module: %s\n",
		    waybar_mod);
		return 1;
	}
	/* `desks` is per-output and needs -o; `title` works without it. */
	if (waybar_mod != NULL && strcmp(waybar_mod, "desks") == 0 &&
	    output_filter == NULL) {
		fprintf(stderr, "cowbar: -w desks requires -o <output>\n");
		return 1;
	}

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		fprintf(stderr, "cowbar: XDG_RUNTIME_DIR not set\n");
		return 1;
	}

	char sock_path[4096];
	snprintf(sock_path, sizeof(sock_path), "%s/cow-status.sock", runtime);

	int sockfd = -1;
	for (int retry = 0; retry < 30; retry++) {
		sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sockfd < 0) {
			perror("cowbar: socket");
			return 1;
		}
		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		if (strlen(sock_path) >= sizeof(addr.sun_path)) {
			fprintf(stderr, "cowbar: socket path too long: %s\n",
			    sock_path);
			close(sockfd);
			return 1;
		}
		strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
		if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) ==
		    0)
			break;
		close(sockfd);
		sockfd = -1;
		sleep(1);
	}
	if (sockfd < 0) {
		fprintf(stderr, "cowbar: cannot connect to %s\n", sock_path);
		return 1;
	}

	FILE *f = fdopen(sockfd, "r");
	if (f == NULL) {
		perror("cowbar: fdopen");
		close(sockfd);
		return 1;
	}
	setlinebuf(f);

	char line[65536];
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		cJSON *state = cJSON_Parse(line);
		if (state == NULL)
			continue;

		if (waybar_mod != NULL) {
			if (strcmp(waybar_mod, "desks") == 0) {
				cJSON *out = find_output(state, output_filter);
				if (out != NULL)
					waybar_desks(state, out);
				else
					emit_waybar("", "");
			} else if (strcmp(waybar_mod, "title") == 0) {
				waybar_title(state, output_filter);
			}
		} else if (field == NULL && output_filter == NULL) {
			/* No filtering -- relay full snapshot */
			emit(state);
		} else if (field != NULL && strcmp(field, "desks") == 0) {
			/* Extract desks for a specific output */
			cJSON *out = find_output(state, output_filter);
			if (out != NULL) {
				cJSON *desks = cJSON_GetObjectItem(out,
				    "desks");
				cJSON *cur   = cJSON_GetObjectItem(out,
				      "current_desk");
				if (desks != NULL) {
					cJSON *result = cJSON_CreateObject();
					cJSON_AddItemReferenceToObject(result,
					    "desks", desks);
					if (cur != NULL)
						cJSON_AddItemReferenceToObject(
						    result, "current_desk",
						    cur);
					cJSON *name = cJSON_GetObjectItem(out,
					    "name");
					if (name != NULL)
						cJSON_AddItemReferenceToObject(
						    result, "output", name);
					emit(result);
					cJSON_Delete(result);
				}
			}
		} else if (field != NULL && strcmp(field, "focused") == 0) {
			cJSON *focused = cJSON_GetObjectItem(state, "focused");
			if (focused != NULL)
				emit(focused);
		} else if (field != NULL && strcmp(field, "outputs") == 0) {
			cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
			if (outputs != NULL)
				emit(outputs);
		} else if (output_filter != NULL && field == NULL) {
			/* Filter to a specific output's full state */
			cJSON *out = find_output(state, output_filter);
			if (out != NULL)
				emit(out);
		}

		cJSON_Delete(state);
	}

	fclose(f);
	return 0;
}
