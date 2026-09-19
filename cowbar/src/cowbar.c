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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cow/module.h"
#include "cow/module_config.h"
#include "cow/module_output.h"
#include "cow/attrs.h"
#include "cow/util.h"

#define PROG "cowbar"
#define MAX_CONFIG_FIELDS 16

enum cowbar_style_id {
	STYLE_DESK_ACTIVE,
	STYLE_DESK_COLLECTED,
	STYLE_DESK_OCCUPIED,
	STYLE_DESK_SEPARATOR,
	STYLE_PAGE_SEPARATOR,
	STYLE_PAGE,
	STYLE_STATUS,
	STYLE_TITLE,
	STYLE_COUNT,
};

enum cowbar_attribute_id {
	ATTR_FOREGROUND,
	ATTR_BACKGROUND,
	ATTR_WEIGHT,
	ATTR_STYLE,
	ATTR_UNDERLINE,
	ATTR_COUNT,
};

enum cowbar_format_id {
	FORMAT_DESK,
	FORMAT_PAGE,
	FORMAT_STATUS,
	FORMAT_TITLE,
	FORMAT_COUNT,
};

enum cowbar_layout_id {
	LAYOUT_DESKS,
	LAYOUT_PAGE,
	LAYOUT_STATUS,
	LAYOUT_COUNT,
};

struct cowbar_span_style {
	char *attributes[ATTR_COUNT];
};

struct cowbar_config {
	struct cowbar_span_style styles[STYLE_COUNT];
	char			 *formats[FORMAT_COUNT];
	enum cowbar_layout_id	  layout[LAYOUT_COUNT];
	int			  layout_count;
};

static const char *style_names[STYLE_COUNT] = {
	[STYLE_DESK_ACTIVE] = "desk.active",
	[STYLE_DESK_COLLECTED] = "desk.collected",
	[STYLE_DESK_OCCUPIED] = "desk.occupied",
	[STYLE_DESK_SEPARATOR] = "desk.separator",
	[STYLE_PAGE_SEPARATOR] = "page.separator",
	[STYLE_PAGE] = "page",
	[STYLE_STATUS] = "status",
	[STYLE_TITLE] = "title",
};

static const char *attribute_names[ATTR_COUNT] = {
	[ATTR_FOREGROUND] = "foreground",
	[ATTR_BACKGROUND] = "background",
	[ATTR_WEIGHT] = "weight",
	[ATTR_STYLE] = "style",
	[ATTR_UNDERLINE] = "underline",
};

static const char *format_names[FORMAT_COUNT] = {
	[FORMAT_DESK] = "desk",
	[FORMAT_PAGE] = "page",
	[FORMAT_STATUS] = "status",
	[FORMAT_TITLE] = "title",
};

static const char *format_conversions[FORMAT_COUNT] = {
	[FORMAT_DESK] = "s",
	[FORMAT_PAGE] = "dd",
	[FORMAT_STATUS] = "sds",
	[FORMAT_TITLE] = "s",
};

static const char *layout_names[LAYOUT_COUNT] = {
	[LAYOUT_DESKS] = "desks",
	[LAYOUT_PAGE] = "page",
	[LAYOUT_STATUS] = "status",
};

static struct cowbar_config config;
static volatile sig_atomic_t reload_requested;
static int reload_pipe[2] = { -1, -1 };

static int
named_index(const char *name, const char *const *names, int count)
{
	for (int i = 0; i < count; i++) {
		if (strcmp(name, names[i]) == 0)
			return i;
	}
	return -1;
}

static bool
format_valid(const char *format, const char *conversions)
{
	size_t conversion = 0;

	for (const char *p = format; *p != '\0'; p++) {
		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		if (*p == '\0' || conversions[conversion] == '\0' ||
		    *p != conversions[conversion])
			return false;
		conversion++;
	}
	return conversions[conversion] == '\0';
}

static bool
config_line(const struct cow_module_config_line *line, const char **why,
    void *data)
{
	struct cowbar_config *cfg = data;
	char		    **fields = line->fields;
	int		      n = line->nfields;

	if (strcmp(fields[0], "style") == 0) {
		if (n < 4 || n % 2 != 0) {
			*why = "style requires a name and attribute/value pairs";
			return false;
		}
		int style = named_index(fields[1], style_names, STYLE_COUNT);
		if (style < 0) {
			*why = "unknown style";
			return false;
		}
		for (int i = 2; i < n; i += 2) {
			int attribute = named_index(fields[i], attribute_names,
			    ATTR_COUNT);
			if (attribute < 0) {
				*why = "unknown style attribute";
				return false;
			}
			char *value = strcmp(fields[i + 1], "none") == 0 ?
			    NULL : strdup(fields[i + 1]);
			if (value == NULL && strcmp(fields[i + 1], "none") != 0) {
				*why = "out of memory";
				return false;
			}
			free(cfg->styles[style].attributes[attribute]);
			cfg->styles[style].attributes[attribute] = value;
		}
		return true;
	}

	if (strcmp(fields[0], "format") == 0) {
		if (n != 3) {
			*why = "format requires a name and value";
			return false;
		}
		int format = named_index(fields[1], format_names, FORMAT_COUNT);
		if (format < 0) {
			*why = "unknown format";
			return false;
		}
		if (!format_valid(fields[2], format_conversions[format])) {
			*why = "format has invalid conversions";
			return false;
		}
		char *value = strdup(fields[2]);
		if (value == NULL) {
			*why = "out of memory";
			return false;
		}
		free(cfg->formats[format]);
		cfg->formats[format] = value;
		return true;
	}

	if (strcmp(fields[0], "layout") == 0) {
		if (n < 2 || n > LAYOUT_COUNT + 1) {
			*why = "layout requires one to three elements";
			return false;
		}
		enum cowbar_layout_id layout[LAYOUT_COUNT];
		for (int i = 1; i < n; i++) {
			int element = named_index(fields[i], layout_names,
			    LAYOUT_COUNT);
			if (element < 0) {
				*why = "unknown layout element";
				return false;
			}
			for (int j = 0; j < i - 1; j++) {
				if (layout[j] == (enum cowbar_layout_id)element) {
					*why = "duplicate layout element";
					return false;
				}
			}
			layout[i - 1] = (enum cowbar_layout_id)element;
		}
		memcpy(cfg->layout, layout, (size_t)(n - 1) * sizeof(*layout));
		cfg->layout_count = n - 1;
		return true;
	}

	*why = "unknown command";
	return false;
}

static bool
config_init(struct cowbar_config *cfg)
{
	static const char *fallbacks[FORMAT_COUNT] = {
		[FORMAT_DESK] = "%s",
		[FORMAT_PAGE] = "[%d, %d]",
		[FORMAT_STATUS] = "[Scr:%s][A:%d][L:%s]",
		[FORMAT_TITLE] = "%s",
	};

	for (int i = 0; i < FORMAT_COUNT; i++) {
		cfg->formats[i] = strdup(fallbacks[i]);
		if (cfg->formats[i] == NULL)
			return false;
	}
	for (int i = 0; i < LAYOUT_COUNT; i++)
		cfg->layout[i] = i;
	cfg->layout_count = LAYOUT_COUNT;
	return true;
}

static void
config_free(struct cowbar_config *cfg)
{
	for (int i = 0; i < STYLE_COUNT; i++) {
		for (int j = 0; j < ATTR_COUNT; j++)
			free(cfg->styles[i].attributes[j]);
	}
	for (int i = 0; i < FORMAT_COUNT; i++)
		free(cfg->formats[i]);
}

static bool
load_config_file(const char *path, bool required, struct cowbar_config *cfg)
{
	if (access(path, R_OK) != 0) {
		if (!required && errno == ENOENT)
			return true;
		fprintf(stderr, "%s: %s: %s\n", PROG, path, strerror(errno));
		return false;
	}
	return cow_module_config_load(PROG, path, MAX_CONFIG_FIELDS,
	    config_line, cfg);
}

static bool
load_config(const char *explicit_path, struct cowbar_config *cfg)
{
	if (!load_config_file(COW_SYSCONFDIR "/cowbar.conf", false, cfg))
		return false;
	if (explicit_path != NULL)
		return load_config_file(explicit_path, true, cfg);

	char *user_path = cow_module_config_default_path(PROG);
	if (user_path == NULL)
		return true;
	bool ok = load_config_file(user_path, false, cfg);
	free(user_path);
	return ok;
}

static bool
config_reload(const char *explicit_path)
{
	struct cowbar_config next = { 0 };

	if (!config_init(&next) || !load_config(explicit_path, &next)) {
		config_free(&next);
		return false;
	}
	struct cowbar_config previous = config;
	config = next;
	config_free(&previous);
	return true;
}

static void
reload_handler(__unused int signal_number)
{
	int saved_errno = errno;
	char byte = 0;

	reload_requested = 1;
	(void)write(reload_pipe[1], &byte, sizeof(byte));
	errno = saved_errno;
}

static bool
install_reload_handler(void)
{
	if (pipe(reload_pipe) < 0)
		return false;
	for (int i = 0; i < 2; i++) {
		int flags = fcntl(reload_pipe[i], F_GETFL);
		if (flags < 0 || fcntl(reload_pipe[i], F_SETFL,
		    flags | O_NONBLOCK) < 0)
			goto fail;
		flags = fcntl(reload_pipe[i], F_GETFD);
		if (flags < 0 || fcntl(reload_pipe[i], F_SETFD,
		    flags | FD_CLOEXEC) < 0)
			goto fail;
	}
	struct sigaction action = { .sa_handler = reload_handler };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGHUP, &action, NULL) < 0)
		goto fail;
	return true;

fail:
	close(reload_pipe[0]);
	close(reload_pipe[1]);
	reload_pipe[0] = reload_pipe[1] = -1;
	return false;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: cowbar [-c path] [-o output] [-f field] [-w module] [-n]\n"
	    "\n"
	    "  -c path     load style and format overrides from path\n"
	    "  -o output   filter by output name or @ selector\n"
	    "  -f field    extract field: desks, focused, language, outputs\n"
	    "  -w module   emit waybar JSON for module: desks, title\n"
	    "              (requires -o for desks; -o filters title to one bar)\n"
	    "  -n          show desk names instead of numbers\n"
	    "\n"
	    "With no flags, relays full state snapshots.\n");
}

/* Find an output by selector. NULL retains cowbar's first-output default. */
static cJSON *
find_output(cJSON *state, const char *name)
{
	cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
	if (!cJSON_IsArray(outputs))
		return NULL;
	if (name == NULL)
		return cJSON_GetArrayItem(outputs, 0);
	return cow_module_output_resolve(outputs, name);
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

/* Emit only when OBJ differs from the last value emitted through PREVIOUS. */
static void
emit_changed(cJSON *obj, char **previous)
{
	char *s = cJSON_PrintUnformatted(obj);
	if (s == NULL)
		return;
	if (*previous != NULL && strcmp(*previous, s) == 0) {
		cJSON_free(s);
		return;
	}
	puts(s);
	fflush(stdout);
	cJSON_free(*previous);
	*previous = s;
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

static char *
style_text(enum cowbar_style_id style_id, const char *text)
{
	char *escaped = cow_markup_escape(text);
	if (escaped == NULL)
		return NULL;

	struct cowbar_span_style *style = &config.styles[style_id];
	bool has_attributes = false;
	for (int i = 0; i < ATTR_COUNT; i++) {
		if (style->attributes[i] != NULL) {
			has_attributes = true;
			break;
		}
	}
	if (!has_attributes)
		return escaped;

	char  *markup = NULL;
	size_t cap = 0, len = 0;
	buf_appendf(&markup, &cap, &len, "<span");
	for (int i = 0; i < ATTR_COUNT; i++) {
		if (style->attributes[i] == NULL)
			continue;
		char *value = cow_markup_escape(style->attributes[i]);
		if (value == NULL) {
			free(markup);
			free(escaped);
			return NULL;
		}
		buf_appendf(&markup, &cap, &len, " %s=\"%s\"",
		    attribute_names[i], value);
		free(value);
	}
	buf_appendf(&markup, &cap, &len, ">%s</span>", escaped);
	free(escaped);
	return markup;
}

static char *
format_desk(const char *label, enum cowbar_style_id style)
{
	char *plain;
	if (asprintf(&plain, config.formats[FORMAT_DESK], label) < 0)
		return NULL;
	char *markup = style_text(style, plain);
	free(plain);
	return markup;
}

static char *
format_page(int col, int row)
{
	char *plain;
	if (asprintf(&plain, config.formats[FORMAT_PAGE], col, row) < 0)
		return NULL;
	char *markup = style_text(STYLE_PAGE, plain);
	free(plain);
	return markup;
}

static char *
format_status(const char *output, int windows, const char *configuration)
{
	char *plain;
	if (asprintf(&plain, config.formats[FORMAT_STATUS], output, windows,
	    configuration) < 0)
		return NULL;
	char *markup = style_text(STYLE_STATUS, plain);
	free(plain);
	return markup;
}

static char *
format_title(const char *title)
{
	char *plain;
	if (asprintf(&plain, config.formats[FORMAT_TITLE], title) < 0)
		return NULL;
	char *markup = style_text(STYLE_TITLE, plain);
	free(plain);
	return markup;
}

/* Emit a Waybar JSON object. Tooltip may be omitted. */
static void
emit_waybar(const char *text, const char *tooltip)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "text", text != NULL ? text : "");
	if (tooltip != NULL)
		cJSON_AddStringToObject(obj, "tooltip", tooltip);
	emit(obj);
	cJSON_Delete(obj);
}

/* Render the waybar 'desks' module text for the given output.  The
 * output object is the per-output entry from cow's state. */
static void
waybar_desks(const cJSON *state, const cJSON *out_obj, bool show_names)
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
		cJSON *nr_j   = cJSON_GetObjectItem(desk, "nr");
		cJSON *win_j  = cJSON_GetObjectItem(desk, "windows");
		cJSON *name_j = cJSON_GetObjectItem(desk, "name");
		int    nr = cJSON_IsNumber(nr_j) ? (int)nr_j->valuedouble : 0;
		int wins  = cJSON_IsNumber(win_j) ? (int)win_j->valuedouble : 0;
		const char *nm = (name_j && cJSON_IsString(name_j)) ?
		    name_j->valuestring : NULL;

		if (active)
			active_wins = wins;
		if (!active && !collected && wins == 0)
			continue;

		if (!first_span) {
			char *separator = style_text(STYLE_DESK_SEPARATOR, "|");
			if (separator != NULL) {
				buf_appendf(&text, &cap, &len, "%s", separator);
				free(separator);
			}
		}
		char label[32];
		const char *desk_label;
		if (show_names && nm != NULL && nm[0] != '\0') {
			desk_label = nm;
		} else {
			snprintf(label, sizeof(label), "%d", nr);
			desk_label = label;
		}
		enum cowbar_style_id style = active ? STYLE_DESK_ACTIVE :
		    collected ? STYLE_DESK_COLLECTED : STYLE_DESK_OCCUPIED;
		char *desk_text = format_desk(desk_label, style);
		if (desk_text != NULL) {
			buf_appendf(&text, &cap, &len, "%s", desk_text);
			free(desk_text);
		}
		first_span = false;
	}

	cJSON *page_col = cJSON_GetObjectItem(out_obj, "page_col");
	cJSON *page_row = cJSON_GetObjectItem(out_obj, "page_row");
	cJSON *name	= cJSON_GetObjectItem(out_obj, "name");
	cJSON *cfg	= cJSON_GetObjectItem(state, "desktop_configuration");

	char *desks = text;
	text = NULL;
	cap = len = 0;
	char *separator = style_text(STYLE_PAGE_SEPARATOR, "|");
	char *page_text = format_page(
	    page_col && cJSON_IsNumber(page_col) ? (int)page_col->valuedouble : 0,
	    page_row && cJSON_IsNumber(page_row) ? (int)page_row->valuedouble : 0);
	char *status = format_status(
	    name && cJSON_IsString(name) ? name->valuestring : "", active_wins,
	    cfg && cJSON_IsString(cfg) ? cfg->valuestring : "");
	char *page = NULL;
	size_t page_cap = 0, page_len = 0;
	if (separator != NULL)
		buf_appendf(&page, &page_cap, &page_len, "%s", separator);
	if (page_text != NULL)
		buf_appendf(&page, &page_cap, &page_len, "%s", page_text);

	char *segments[LAYOUT_COUNT] = {
		[LAYOUT_DESKS] = desks,
		[LAYOUT_PAGE] = page,
		[LAYOUT_STATUS] = status,
	};
	for (int i = 0; i < config.layout_count; i++) {
		const char *segment = segments[config.layout[i]];
		if (segment != NULL)
			buf_appendf(&text, &cap, &len, "%s", segment);
	}
	free(separator);
	free(page_text);
	free(desks);
	free(page);
	free(status);

	cJSON *cur_desk = cJSON_GetObjectItem(out_obj, "current_desk");
	const char *cur_name = NULL;
	cJSON_ArrayForEach(desk, desks_arr)
	{
		if (cJSON_IsTrue(cJSON_GetObjectItem(desk, "active"))) {
			cJSON *nm = cJSON_GetObjectItem(desk, "name");
			if (cJSON_IsString(nm) && nm->valuestring[0] != '\0')
				cur_name = nm->valuestring;
			break;
		}
	}
	char tooltip[128];
	int  cur_nr = cur_desk && cJSON_IsNumber(cur_desk) ?
		   (int)cur_desk->valuedouble :
		   0;
	if (cur_name != NULL)
		snprintf(tooltip, sizeof(tooltip), "desk %d (%s)", cur_nr,
		    cur_name);
	else
		snprintf(tooltip, sizeof(tooltip), "desk %d", cur_nr);

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

	cJSON *title_j = cJSON_GetObjectItem(focused, "display_title");
	cJSON *app_j	= cJSON_GetObjectItem(focused, "app_id");
	const char *title = cJSON_IsString(title_j) ? title_j->valuestring : "";
	char	   *text = format_title(title);
	emit_waybar(text != NULL ? text : "",
	    cJSON_IsString(app_j) ? app_j->valuestring : "");
	free(text);
}

static bool
process_state(const char *line, const char *waybar_mod,
    const char *output_filter, const char *field, bool show_names,
    char **last_language)
{
	cJSON *state = cJSON_Parse(line);
	if (state == NULL)
		return false;

	if (waybar_mod != NULL) {
		if (strcmp(waybar_mod, "desks") == 0) {
			cJSON *out = find_output(state, output_filter);
			if (out != NULL)
				waybar_desks(state, out, show_names);
			else
				emit_waybar("", "");
		} else if (strcmp(waybar_mod, "title") == 0) {
			const char *resolved = output_filter;
			if (output_filter != NULL) {
				cJSON *output = find_output(state, output_filter);
				cJSON *name = output != NULL ?
				    cJSON_GetObjectItem(output, "name") : NULL;
				resolved = cJSON_IsString(name) ?
				    name->valuestring : "";
			}
			waybar_title(state, resolved);
		}
	} else if (field == NULL && output_filter == NULL) {
		emit(state);
	} else if (field != NULL && strcmp(field, "desks") == 0) {
		cJSON *out = find_output(state, output_filter);
		if (out != NULL) {
			cJSON *desks = cJSON_GetObjectItem(out, "desks");
			cJSON *cur = cJSON_GetObjectItem(out, "current_desk");
			if (desks != NULL) {
				cJSON *result = cJSON_CreateObject();
				cJSON_AddItemReferenceToObject(result, "desks", desks);
				if (cur != NULL)
					cJSON_AddItemReferenceToObject(result,
					    "current_desk", cur);
				cJSON *name = cJSON_GetObjectItem(out, "name");
				if (name != NULL)
					cJSON_AddItemReferenceToObject(result,
					    "output", name);
				emit(result);
				cJSON_Delete(result);
			}
		}
	} else if (field != NULL && strcmp(field, "focused") == 0) {
		cJSON *focused = cJSON_GetObjectItem(state, "focused");
		if (focused != NULL)
			emit(focused);
	} else if (field != NULL && strcmp(field, "language") == 0) {
		cJSON *language = cJSON_GetObjectItem(state, "language");
		cJSON *name = cJSON_GetObjectItem(language, "name");
		cJSON *caps_lock = cJSON_GetObjectItem(language, "caps_lock");
		cJSON *num_lock = cJSON_GetObjectItem(language, "num_lock");
		cJSON *scroll_lock = cJSON_GetObjectItem(language, "scroll_lock");
		cJSON *result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "text",
		    cJSON_IsString(name) ? name->valuestring : "");
		if (cJSON_IsBool(caps_lock))
			cJSON_AddBoolToObject(result, "caps_lock",
			    cJSON_IsTrue(caps_lock));
		else
			cJSON_AddNullToObject(result, "caps_lock");
		if (cJSON_IsBool(num_lock))
			cJSON_AddBoolToObject(result, "num_lock",
			    cJSON_IsTrue(num_lock));
		else
			cJSON_AddNullToObject(result, "num_lock");
		if (cJSON_IsBool(scroll_lock))
			cJSON_AddBoolToObject(result, "scroll_lock",
			    cJSON_IsTrue(scroll_lock));
		else
			cJSON_AddNullToObject(result, "scroll_lock");
		emit_changed(result, last_language);
		cJSON_Delete(result);
	} else if (field != NULL && strcmp(field, "outputs") == 0) {
		cJSON *outputs = cJSON_GetObjectItem(state, "outputs");
		if (outputs != NULL)
			emit(outputs);
	} else if (output_filter != NULL && field == NULL) {
		cJSON *out = find_output(state, output_filter);
		if (out != NULL)
			emit(out);
	}

	cJSON_Delete(state);
	return true;
}

int
main(int argc, char *argv[])
{
	const char *config_path   = NULL;
	const char *output_filter = NULL;
	const char *field	  = NULL;
	const char *waybar_mod	  = NULL;
	bool	    show_names	  = false;
	int	    opt;

	if (!config_init(&config)) {
		fprintf(stderr, "%s: out of memory\n", PROG);
		return 1;
	}

	while ((opt = getopt(argc, argv, "c:o:f:w:nh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 'o':
			output_filter = optarg;
			break;
		case 'f':
			field = optarg;
			break;
		case 'w':
			waybar_mod = optarg;
			break;
		case 'n':
			show_names = true;
			break;
		case 'h':
		default:
			usage();
			config_free(&config);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (waybar_mod != NULL && strcmp(waybar_mod, "desks") != 0 &&
	    strcmp(waybar_mod, "title") != 0) {
		fprintf(stderr, "cowbar: unknown waybar module: %s\n",
		    waybar_mod);
		config_free(&config);
		return 1;
	}
	/* `desks` is per-output and needs -o; `title` works without it. */
	if (waybar_mod != NULL && strcmp(waybar_mod, "desks") == 0 &&
	    output_filter == NULL) {
		fprintf(stderr, "cowbar: -w desks requires -o <output>\n");
		config_free(&config);
		return 1;
	}
	if (!load_config(config_path, &config)) {
		config_free(&config);
		return 1;
	}
	if (!install_reload_handler()) {
		fprintf(stderr, "%s: cannot install SIGHUP handler: %s\n", PROG,
		    strerror(errno));
		config_free(&config);
		return 1;
	}

	int sockfd = cow_module_status_connect("cowbar",
	    COW_MODULE_STATUS_RETRIES, COW_MODULE_STATUS_RETRY_DELAY);
	if (sockfd < 0) {
		close(reload_pipe[0]);
		close(reload_pipe[1]);
		config_free(&config);
		return 1;
	}

	FILE *f = fdopen(sockfd, "r");
	if (f == NULL) {
		perror("cowbar: fdopen");
		close(sockfd);
		close(reload_pipe[0]);
		close(reload_pipe[1]);
		config_free(&config);
		return 1;
	}
	setvbuf(f, NULL, _IONBF, 0);

	char *line = NULL;
	size_t line_capacity = 0;
	char *last_line = NULL;
	char *last_language = NULL;
	for (;;) {
		struct pollfd fds[] = {
			{ .fd = sockfd, .events = POLLIN },
			{ .fd = reload_pipe[0], .events = POLLIN },
		};
		int ready;
		do {
			ready = poll(fds, 2, -1);
		} while (ready < 0 && errno == EINTR);
		if (ready < 0) {
			fprintf(stderr, "%s: poll: %s\n", PROG, strerror(errno));
			break;
		}

		if (fds[1].revents & POLLIN) {
			char bytes[64];
			while (read(reload_pipe[0], bytes, sizeof(bytes)) > 0)
				;
		}
		if (reload_requested) {
			reload_requested = 0;
			if (config_reload(config_path)) {
				fprintf(stderr, "%s: configuration reloaded\n", PROG);
				if (waybar_mod != NULL && last_line != NULL)
					process_state(last_line, waybar_mod,
					    output_filter, field, show_names,
					    &last_language);
			} else {
				fprintf(stderr, "%s: reload failed; keeping current "
				    "configuration\n", PROG);
			}
		}

		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (!(fds[0].revents & POLLIN))
			continue;
		ssize_t line_length = getline(&line, &line_capacity, f);
		if (line_length < 0)
			break;
		if (line_length > 0 && line[line_length - 1] == '\n')
			line[--line_length] = '\0';
		if (!process_state(line, waybar_mod, output_filter, field,
		    show_names, &last_language))
			continue;
		char *copy = strdup(line);
		if (copy != NULL) {
			free(last_line);
			last_line = copy;
		}
	}

	free(last_line);
	free(line);
	cJSON_free(last_language);
	fclose(f);
	close(reload_pipe[0]);
	close(reload_pipe[1]);
	config_free(&config);
	return 0;
}
