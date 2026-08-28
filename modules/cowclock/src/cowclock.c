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

#include <cairo.h>
#include <cJSON.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/colour.h"
#include "cow/module.h"
#include "cow/module_config.h"
#include "cow/module_draw.h"
#include "cow/module_shm.h"
#include "cow/module_theme.h"
#include "cow/text.h"
#include "xdg-shell-client-protocol.h"

#define PROG "cowclock"
#define INITIAL_SIZE 360
#define MAX_FIELDS 4
#define TAU (2.0 * M_PI)

enum numeral_style {
	NUMERALS_NONE,
	NUMERALS_ARABIC,
	NUMERALS_ROMAN,
};

enum second_mode {
	SECOND_NONE,
	SECOND_TICK,
	SECOND_SMOOTH,
};

enum colour_slot {
	COLOUR_BACKGROUND,
	COLOUR_FOREGROUND,
	COLOUR_TICKS,
	COLOUR_NUMERALS,
	COLOUR_HOUR,
	COLOUR_MINUTE,
	COLOUR_SECOND,
	COLOUR_CENTRE,
	COLOUR_OUTLINE,
	COLOUR_COUNT,
};

struct hand_geometry {
	double length;
	double width;
};

struct tick_geometry {
	double length;
	double width;
};

struct clock_config {
	int width;
	int height;
	int min_width;
	int min_height;
	int padding;
	bool ticks;
	bool aspect_ratio;
	bool clockwise;
	enum numeral_style numerals;
	enum second_mode second_mode;
	char font[256];
	char timezone[128];
	char date_format[128];
	double numeral_radius;
	double centre_size;
	double outline_width;
	struct hand_geometry hour;
	struct hand_geometry minute;
	struct hand_geometry second;
	struct tick_geometry major_tick;
	struct tick_geometry minor_tick;
	float background[4];
	float foreground[4];
	float ticks_colour[4];
	float numerals_colour[4];
	float hour_colour[4];
	float minute_colour[4];
	float second_colour[4];
	float centre_colour[4];
	float outline_colour[4];
	bool colour_set[COLOUR_COUNT];
	bool font_set;
};

static struct clock_config config;
static struct clock_config base_config;
static const char *explicit_config;
static char *initial_timezone;
static bool initial_timezone_set;
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *wl_shm;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct cow_module_shm_buffer buffers[2];
static struct event_base *event_base;
static struct event *timer_event;
static struct bufferevent *status_bev;
static struct wl_callback *frame_callback;
static struct cow_module_theme cow_theme;
static struct cow_module_theme titlebar_theme;
static int width = INITIAL_SIZE;
static int height = INITIAL_SIZE;
static bool configured;
static bool running = true;
static bool tick_pending;

static void render(void);

static void
frame_done(__unused void *data, struct wl_callback *callback,
    __unused uint32_t time)
{
	wl_callback_destroy(callback);
	frame_callback = NULL;

	if (config.second_mode == SECOND_SMOOTH) {
		render();
		return;
	}
	if (config.second_mode == SECOND_TICK && tick_pending) {
		tick_pending = false;
		render();
	}
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void
request_frame(bool commit)
{
	if (frame_callback != NULL)
		return;

	frame_callback = wl_surface_frame(surface);
	wl_callback_add_listener(frame_callback, &frame_listener, NULL);

	if (commit)
		wl_surface_commit(surface);
}

static void
minimum_dimensions(const struct clock_config *cfg, int *min_width,
    int *min_height)
{
	*min_width = cfg->min_width;
	*min_height = cfg->min_height;
	if (!cfg->aspect_ratio)
		return;

	double ratio = (double)cfg->width / cfg->height;

	if ((double)*min_width / *min_height > ratio)
		*min_height = (int)ceil(*min_width / ratio);
	else
		*min_width = (int)ceil(*min_height * ratio);
}

static void
config_defaults(struct clock_config *cfg)
{
	*cfg = (struct clock_config) {
		.width = INITIAL_SIZE,
		.height = INITIAL_SIZE,
		.min_width = 120,
		.min_height = 120,
		.padding = 14,
		.ticks = true,
		.aspect_ratio = true,
		.clockwise = true,
		.numerals = NUMERALS_ROMAN,
		.second_mode = SECOND_TICK,
		.numeral_radius = 72.0,
		.centre_size = 2.5,
		.outline_width = 0.0,
		.hour = { .length = 52.0, .width = 11.0 },
		.minute = { .length = 72.0, .width = 7.0 },
		.second = { .length = 82.0, .width = 0.9 },
		.major_tick = { .length = 10.0, .width = 0.9 },
		.minor_tick = { .length = 4.0, .width = 0.4 },
	};

	strlcpy(cfg->font, "serif 13", sizeof(cfg->font));
	strlcpy(cfg->timezone, "local", sizeof(cfg->timezone));
	
	cow_colour_parse("0xffffffff", cfg->background);
	cow_colour_parse("0x000000ff", cfg->foreground);
	cow_colour_parse("0x000000ff", cfg->ticks_colour);
	cow_colour_parse("0x000000ff", cfg->numerals_colour);
	cow_colour_parse("0x000000ff", cfg->hour_colour);
	cow_colour_parse("0x000000ff", cfg->minute_colour);
	cow_colour_parse("0xc02020ff", cfg->second_colour);
	cow_colour_parse("0x000000ff", cfg->centre_colour);
	cow_colour_parse("0x000000ff", cfg->outline_colour);
}

static bool
parse_double(const char *text, double *value)
{
	char *end;
	errno = 0;
	double parsed = strtod(text, &end);
	
	if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
		return false;
	
	*value = parsed;
	return true;
}

static bool
parse_style(struct clock_config *cfg, char **fields, int n,
    const char **why)
{
	if (n != 3) {
		*why = "usage: style NAME COLOUR";
		return false;
	}

	float *colour;
	enum colour_slot slot;
	if (strcmp(fields[1], "background") == 0) {
		slot = COLOUR_BACKGROUND;
		colour = cfg->background;
	} else if (strcmp(fields[1], "foreground") == 0) {
		slot = COLOUR_FOREGROUND;
		colour = cfg->foreground;
	} else if (strcmp(fields[1], "ticks") == 0) {
		slot = COLOUR_TICKS;
		colour = cfg->ticks_colour;
	} else if (strcmp(fields[1], "numerals") == 0) {
		slot = COLOUR_NUMERALS;
		colour = cfg->numerals_colour;
	} else if (strcmp(fields[1], "hour") == 0) {
		slot = COLOUR_HOUR;
		colour = cfg->hour_colour;
	} else if (strcmp(fields[1], "minute") == 0) {
		slot = COLOUR_MINUTE;
		colour = cfg->minute_colour;
	} else if (strcmp(fields[1], "second") == 0) {
		slot = COLOUR_SECOND;
		colour = cfg->second_colour;
	} else if (strcmp(fields[1], "centre") == 0) {
		slot = COLOUR_CENTRE;
		colour = cfg->centre_colour;
	} else if (strcmp(fields[1], "outline") == 0) {
		slot = COLOUR_OUTLINE;
		colour = cfg->outline_colour;
	} else {
		*why = "unknown style";
		return false;
	}

	if (!cow_colour_parse(fields[2], colour)) {
		*why = "invalid colour";
		return false;
	}
	cfg->colour_set[slot] = true;

	if (slot == COLOUR_FOREGROUND) {
		enum colour_slot slots[] = { COLOUR_TICKS, COLOUR_NUMERALS,
			COLOUR_HOUR, COLOUR_MINUTE, COLOUR_SECOND,
			COLOUR_CENTRE, COLOUR_OUTLINE };
		float *targets[] = { cfg->ticks_colour, cfg->numerals_colour,
			cfg->hour_colour, cfg->minute_colour,
			cfg->second_colour, cfg->centre_colour,
			cfg->outline_colour };
		for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
			if (!cfg->colour_set[slots[i]])
				memcpy(targets[i], colour,
				    sizeof(cfg->foreground));
		}
	}
	return true;
}

static bool
config_line(const struct cow_module_config_line *line, const char **why,
    void *data)
{
	struct clock_config *cfg = data;
	char **fields = line->fields;
	int n = line->nfields;
	int value;
	bool enabled;

	if (strcmp(fields[0], "size") == 0) {
		if (n != 3 ||
		    !cow_module_config_parse_int(fields[1], &cfg->width) ||
		    !cow_module_config_parse_int(fields[2], &cfg->height) ||
		    cfg->width < 64 || cfg->height < 64) {
			*why = "size requires WIDTH HEIGHT of at least 64";
			return false;
		}
	} else if (strcmp(fields[0], "minimum-size") == 0) {
		if (n != 3 ||
		    !cow_module_config_parse_int(fields[1], &cfg->min_width) ||
		    !cow_module_config_parse_int(fields[2], &cfg->min_height) ||
		    cfg->min_width < 1 || cfg->min_height < 1) {
			*why = "minimum-size requires positive WIDTH HEIGHT";
			return false;
		}
	} else if (strcmp(fields[0], "padding") == 0) {
		if (n != 2 ||
		    !cow_module_config_parse_int(fields[1], &value) || value < 0) {
			*why = "padding requires a non-negative integer";
			return false;
		}
		cfg->padding = value;
	} else if (strcmp(fields[0], "font") == 0) {
		if (n != 2 || fields[1][0] == '\0') {
			*why = "font requires a Pango font description";
			return false;
		}
		strlcpy(cfg->font, fields[1], sizeof(cfg->font));
		cfg->font_set = true;
	} else if (strcmp(fields[0], "timezone") == 0) {
		if (n != 2 || fields[1][0] == '\0') {
			*why = "timezone requires local or a timezone name";
			return false;
		}
		strlcpy(cfg->timezone, fields[1], sizeof(cfg->timezone));
	} else if (strcmp(fields[0], "date-format") == 0) {
		if (n != 2) {
			*why = "date-format requires one quoted string";
			return false;
		}
		strlcpy(cfg->date_format, fields[1], sizeof(cfg->date_format));
	} else if (strcmp(fields[0], "ticks") == 0 ||
	    strcmp(fields[0], "aspect-ratio") == 0) {
		if (n != 2 || !cow_module_config_parse_bool(fields[1], &enabled)) {
			*why = "option requires true or false";
			return false;
		}
		if (strcmp(fields[0], "ticks") == 0)
			cfg->ticks = enabled;
		else
			cfg->aspect_ratio = enabled;
	} else if (strcmp(fields[0], "second-mode") == 0) {
		if (n != 2) {
			*why = "second-mode requires tick, smooth, or none";
			return false;
		}
		if (strcmp(fields[1], "tick") == 0)
			cfg->second_mode = SECOND_TICK;
		else if (strcmp(fields[1], "smooth") == 0)
			cfg->second_mode = SECOND_SMOOTH;
		else if (strcmp(fields[1], "none") == 0)
			cfg->second_mode = SECOND_NONE;
		else {
			*why = "second-mode requires tick, smooth, or none";
			return false;
		}
	} else if (strcmp(fields[0], "hand") == 0) {
		struct hand_geometry *hand = NULL;
		if (n == 4 && strcmp(fields[1], "hour") == 0)
			hand = &cfg->hour;
		else if (n == 4 && strcmp(fields[1], "minute") == 0)
			hand = &cfg->minute;
		else if (n == 4 && strcmp(fields[1], "second") == 0)
			hand = &cfg->second;
		if (hand == NULL || !parse_double(fields[2], &hand->length) ||
		    !parse_double(fields[3], &hand->width) ||
		    hand->length <= 0.0 || hand->length > 100.0 ||
		    hand->width <= 0.0 || hand->width > 50.0) {
			*why = "hand requires hour|minute|second LENGTH WIDTH";
			return false;
		}
	} else if (strcmp(fields[0], "tick") == 0) {
		struct tick_geometry *tick = NULL;
		if (n == 4 && strcmp(fields[1], "major") == 0)
			tick = &cfg->major_tick;
		else if (n == 4 && strcmp(fields[1], "minor") == 0)
			tick = &cfg->minor_tick;
		if (tick == NULL || !parse_double(fields[2], &tick->length) ||
		    !parse_double(fields[3], &tick->width) ||
		    tick->length < 0.0 || tick->length > 100.0 ||
		    tick->width <= 0.0 || tick->width > 10.0) {
			*why = "tick requires major|minor LENGTH WIDTH";
			return false;
		}
	} else if (strcmp(fields[0], "numeral-radius") == 0 ||
	    strcmp(fields[0], "centre-size") == 0 ||
	    strcmp(fields[0], "face-outline") == 0) {
		double *target = strcmp(fields[0], "numeral-radius") == 0 ?
		    &cfg->numeral_radius :
		    strcmp(fields[0], "centre-size") == 0 ?
		    &cfg->centre_size : &cfg->outline_width;
		if (n != 2 || !parse_double(fields[1], target) ||
		    *target < 0.0 || *target > 100.0) {
			*why = "option requires a percentage from 0 to 100";
			return false;
		}
	} else if (strcmp(fields[0], "direction") == 0) {
		if (n != 2 || (strcmp(fields[1], "clockwise") != 0 &&
		    strcmp(fields[1], "anticlockwise") != 0)) {
			*why = "direction requires clockwise or anticlockwise";
			return false;
		}
		cfg->clockwise = strcmp(fields[1], "clockwise") == 0;
	} else if (strcmp(fields[0], "numerals") == 0) {
		if (n != 2) {
			*why = "numerals requires roman, arabic, or none";
			return false;
		}
		if (strcmp(fields[1], "roman") == 0)
			cfg->numerals = NUMERALS_ROMAN;
		else if (strcmp(fields[1], "arabic") == 0)
			cfg->numerals = NUMERALS_ARABIC;
		else if (strcmp(fields[1], "none") == 0)
			cfg->numerals = NUMERALS_NONE;
		else {
			*why = "numerals requires roman, arabic, or none";
			return false;
		}
	} else if (strcmp(fields[0], "style") == 0) {
		return parse_style(cfg, fields, n, why);
	} else {
		*why = "unknown command";
		return false;
	}
	return true;
}

static bool
load_config_file(const char *path, bool required, struct clock_config *cfg)
{
	if (access(path, R_OK) != 0) {
		if (!required && errno == ENOENT)
			return true;
		fprintf(stderr, "%s: %s: %s\n", PROG, path, strerror(errno));
		return false;
	}
	return cow_module_config_load(PROG, path, MAX_FIELDS, config_line, cfg);
}

static bool
load_config(struct clock_config *cfg)
{
	config_defaults(cfg);
	if (!load_config_file(COW_SYSCONFDIR "/cowclock.conf", false, cfg))
		return false;
	if (explicit_config != NULL)
		return load_config_file(explicit_config, true, cfg);

	char *path = cow_module_config_default_path(PROG);
	if (path == NULL)
		return true;
	bool ok = load_config_file(path, false, cfg);
	free(path);
	return ok;
}

static void
apply_timezone(void)
{
	if (strcmp(config.timezone, "local") == 0) {
		if (initial_timezone_set)
			setenv("TZ", initial_timezone, 1);
		else
			unsetenv("TZ");
	} else {
		setenv("TZ", config.timezone, 1);
	}
	tzset();
}

static const float *
theme_background(void)
{
	if (titlebar_theme.has_inactive_bg)
		return titlebar_theme.inactive_bg;
	if (cow_theme.has_inactive_bg)
		return cow_theme.inactive_bg;
	if (cow_theme.has_background)
		return cow_theme.background;
	return NULL;
}

static const float *
theme_foreground(void)
{
	if (titlebar_theme.has_inactive_fg)
		return titlebar_theme.inactive_fg;
	if (cow_theme.has_inactive_fg)
		return cow_theme.inactive_fg;
	if (titlebar_theme.has_active_fg)
		return titlebar_theme.active_fg;
	if (cow_theme.has_active_fg)
		return cow_theme.active_fg;
	return NULL;
}

static void
apply_theme(struct clock_config *cfg)
{
	const float *background = theme_background();
	if (!cfg->colour_set[COLOUR_BACKGROUND] && background != NULL)
		memcpy(cfg->background, background, sizeof(cfg->background));

	const float *foreground = cfg->colour_set[COLOUR_FOREGROUND] ?
	    cfg->foreground : theme_foreground();
	if (foreground == NULL)
		foreground = cfg->foreground;
	else if (!cfg->colour_set[COLOUR_FOREGROUND])
		memcpy(cfg->foreground, foreground, sizeof(cfg->foreground));

	enum colour_slot slots[] = {
		COLOUR_TICKS,
		COLOUR_NUMERALS,
		COLOUR_HOUR,
		COLOUR_MINUTE,
		COLOUR_SECOND,
		COLOUR_CENTRE,
		COLOUR_OUTLINE
	};

	float *targets[] = {
		cfg->ticks_colour,
		cfg->numerals_colour,
		cfg->hour_colour,
		cfg->minute_colour,
		cfg->second_colour,
		cfg->centre_colour,
		cfg->outline_colour
	};
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
		if (!cfg->colour_set[slots[i]])
			memcpy(targets[i], foreground, sizeof(cfg->foreground));

	if (!cfg->font_set && titlebar_theme.has_font)
		strlcpy(cfg->font, titlebar_theme.font, sizeof(cfg->font));
}

static bool
effective_style_equal(const struct clock_config *a,
    const struct clock_config *b)
{
	return strcmp(a->font, b->font) == 0 &&
	    memcmp(a->background, b->background, sizeof(a->background)) == 0 &&
	    memcmp(a->foreground, b->foreground, sizeof(a->foreground)) == 0 &&
	    memcmp(a->ticks_colour, b->ticks_colour,
	    sizeof(a->ticks_colour)) == 0 &&
	    memcmp(a->numerals_colour, b->numerals_colour,
	    sizeof(a->numerals_colour)) == 0 &&
	    memcmp(a->hour_colour, b->hour_colour,
	    sizeof(a->hour_colour)) == 0 &&
	    memcmp(a->minute_colour, b->minute_colour,
	    sizeof(a->minute_colour)) == 0 &&
	    memcmp(a->second_colour, b->second_colour,
	    sizeof(a->second_colour)) == 0 &&
	    memcmp(a->centre_colour, b->centre_colour,
	    sizeof(a->centre_colour)) == 0 &&
	    memcmp(a->outline_colour, b->outline_colour,
	    sizeof(a->outline_colour)) == 0;
}

static bool
refresh_effective_config(void)
{
	struct clock_config next = base_config;
	
	apply_theme(&next);
	bool changed = !effective_style_equal(&next, &config);
	config = next;
	
	return changed;
}

static void
parse_status(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	
	if (root == NULL)
		return;
	
	cJSON *theme = cJSON_GetObjectItem(root, "theme");
	struct cow_module_theme next_theme;
	struct cow_module_theme next_titlebar;
	(void)cow_module_theme_parse(theme, &next_theme);
	(void)cow_module_theme_parse(cJSON_GetObjectItem(theme, "titlebar"),
	    &next_titlebar);
	cow_theme = next_theme;
	titlebar_theme = next_titlebar;
	cJSON_Delete(root);
	
	if (refresh_effective_config())
		render();
}

static void
status_read_cb(struct bufferevent *bev, __unused void *data)
{
	struct evbuffer *input = bufferevent_get_input(bev);
	char *line;
	size_t length;
	while ((line = evbuffer_readln(input, &length,
	    EVBUFFER_EOL_LF)) != NULL) {
		parse_status(line);
		free(line);
	}
}

static void
status_event_cb(__unused struct bufferevent *bev, short events,
	    __unused void *data)
{
	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		bufferevent_free(status_bev);
		status_bev = NULL;
	}
}

static void
connect_status(void)
{
	int fd = cow_module_status_connect(PROG, 1, 0);
	if (fd < 0)
		return;
	status_bev = bufferevent_socket_new(event_base, fd,
	    BEV_OPT_CLOSE_ON_FREE);
	if (status_bev == NULL) {
		close(fd);
		return;
	}
	bufferevent_setcb(status_bev, status_read_cb, NULL, status_event_cb,
	    NULL);
	bufferevent_enable(status_bev, EV_READ);
}

static double
clock_angle(double value, double range)
{
	double direction = config.clockwise ? 1.0 : -1.0;
	return direction * value / range * TAU - M_PI_2;
}

static void
draw_ticks(cairo_t *cr, double cx, double cy, double radius)
{
	if (!config.ticks)
		return;
	cow_module_cairo_set_source(cr, config.ticks_colour);
	for (int i = 0; i < 60; i++) {
		const struct tick_geometry *tick = i % 5 == 0 ?
		    &config.major_tick : &config.minor_tick;
		double angle = clock_angle(i, 60.0);
		double outer = radius * 0.98;
		double inner = outer - radius * tick->length / 100.0;
		cairo_set_line_width(cr,
		    fmax(0.5, radius * tick->width / 100.0));
		cairo_move_to(cr, cx + cos(angle) * inner,
		    cy + sin(angle) * inner);
		cairo_line_to(cr, cx + cos(angle) * outer,
		    cy + sin(angle) * outer);
		cairo_stroke(cr);
	}
}

static PangoFontDescription *
scaled_font(double radius, double factor)
{
	PangoFontDescription *font = pango_font_description_from_string(
	    config.font);
	if (font == NULL)
		return NULL;
	int font_size = pango_font_description_get_size(font);
	if (font_size <= 0)
		font_size = 13 * PANGO_SCALE;
	double base_radius = fmax(1.0,
	    fmin(config.width, config.height) / 2.0 - config.padding);
	double scaled_size = fmax(PANGO_SCALE,
	    font_size * radius / base_radius * factor);
	if (pango_font_description_get_size_is_absolute(font))
		pango_font_description_set_absolute_size(font, scaled_size);
	else
		pango_font_description_set_size(font, (int)lround(scaled_size));
	return font;
}

static void
draw_numerals(cairo_t *cr, double cx, double cy, double radius)
{
	static const char *roman[] = {
		"", "I", "II", "III", "IIII", "V", "VI",
		"VII", "VIII", "IX", "X", "XI", "XII",
	};
	if (config.numerals == NUMERALS_NONE)
		return;

	PangoFontDescription *font = scaled_font(radius, 1.0);
	if (font == NULL)
		return;
	cow_module_cairo_set_source(cr, config.numerals_colour);
	for (int i = 1; i <= 12; i++) {
		char arabic[4];
		const char *text = roman[i];
		if (config.numerals == NUMERALS_ARABIC) {
			snprintf(arabic, sizeof(arabic), "%d", i);
			text = arabic;
		}
		double angle = clock_angle(i, 12.0);
		double numeral_radius = radius * config.numeral_radius / 100.0;
		double x = cx + cos(angle) * numeral_radius;
		double y = cy + sin(angle) * numeral_radius;
		int tw, th;
		cow_text_measure(cr, font, text, &tw, &th);
		int baseline = 0;
		cow_text_render(cr, font, x - tw, y - th / 2.0, tw * 2.0,
		    th, text, PANGO_ALIGN_CENTER, &baseline);
	}
	pango_font_description_free(font);
}

static void
draw_hand(cairo_t *cr, double cx, double cy, double angle, double radius,
	    const struct hand_geometry *geometry, const float colour[4])
{
	double ux = cos(angle);
	double uy = sin(angle);
	double px = -uy;
	double py = ux;
	double half_width = radius * geometry->width / 200.0;
	double back = radius * 0.08;
	double tip = radius * geometry->length / 100.0;

	cow_module_cairo_set_source(cr, colour);
	cairo_move_to(cr, cx + ux * tip, cy + uy * tip);
	cairo_line_to(cr, cx - ux * back + px * half_width,
	    cy - uy * back + py * half_width);
	cairo_line_to(cr, cx - ux * back - px * half_width,
	    cy - uy * back - py * half_width);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void
draw_date(cairo_t *cr, const struct tm *tm, double cx, double cy,
	    double radius)
{
	if (config.date_format[0] == '\0')
		return;
	char text[256];
	if (strftime(text, sizeof(text), config.date_format, tm) == 0)
		return;
	PangoFontDescription *font = scaled_font(radius, 0.8);
	if (font == NULL)
		return;
	cow_module_cairo_set_source(cr, config.foreground);
	int tw, th;
	cow_text_measure(cr, font, text, &tw, &th);
	double y = cy + radius * 0.34 - th / 2.0;
	int baseline = 0;
	cow_text_render(cr, font, cx - radius * 0.45, y, radius * 0.9, th,
	    text, PANGO_ALIGN_CENTER, &baseline);
	pango_font_description_free(font);
}

static void
draw_clock(cairo_t *cr, const struct tm *tm, double fraction,
	    double cx, double cy, double radius)
{
	if (config.outline_width > 0.0) {
		cow_module_cairo_set_source(cr, config.outline_colour);
		cairo_set_line_width(cr,
		    fmax(0.5, radius * config.outline_width / 100.0));
		cairo_arc(cr, cx, cy, radius * 0.98, 0, TAU);
		cairo_stroke(cr);
	}
	draw_ticks(cr, cx, cy, radius);
	draw_numerals(cr, cx, cy, radius);
	draw_date(cr, tm, cx, cy, radius);

	double seconds = tm->tm_sec + fraction;
	double minutes = tm->tm_min + seconds / 60.0;
	double hours = tm->tm_hour % 12 + minutes / 60.0;
	draw_hand(cr, cx, cy, clock_angle(hours, 12.0), radius, &config.hour,
	    config.hour_colour);
	draw_hand(cr, cx, cy, clock_angle(minutes, 60.0), radius,
	    &config.minute, config.minute_colour);
	if (config.second_mode != SECOND_NONE) {
		cow_module_cairo_set_source(cr, config.second_colour);
		cairo_set_line_width(cr,
		    fmax(0.5, radius * config.second.width / 100.0));
		double angle = clock_angle(seconds, 60.0);
		cairo_move_to(cr, cx - cos(angle) * radius * 0.12,
		    cy - sin(angle) * radius * 0.12);
		double length = radius * config.second.length / 100.0;
		cairo_line_to(cr, cx + cos(angle) * length,
		    cy + sin(angle) * length);
		cairo_stroke(cr);
	}
	cow_module_cairo_set_source(cr, config.centre_colour);
	if (config.centre_size > 0.0) {
		cairo_arc(cr, cx, cy,
		    radius * config.centre_size / 100.0, 0, TAU);
		cairo_fill(cr);
	}
}

static void
render(void)
{
	if (!configured || width <= 0 || height <= 0)
		return;
	struct cow_module_shm_buffer *buf = cow_module_shm_buffer_next(buffers,
	    sizeof(buffers) / sizeof(buffers[0]), wl_shm, PROG, width, height);
	if (buf == NULL)
		return;
	cairo_surface_t *image = cairo_image_surface_create_for_data(buf->data,
	    CAIRO_FORMAT_ARGB32, width, height, buf->stride);
	cairo_t *cr = cairo_create(image);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cow_module_cairo_set_source(cr, config.background);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t seconds = now.tv_sec;
	struct tm tm;
	localtime_r(&seconds, &tm);
	double radius = fmax(1.0, fmin(width, height) / 2.0 - config.padding);
	double fraction = config.second_mode == SECOND_SMOOTH ?
	    now.tv_nsec / 1000000000.0 : 0.0;
	draw_clock(cr, &tm, fraction, width / 2.0,
	    height / 2.0, radius);

	cairo_destroy(cr);
	cairo_surface_destroy(image);
	if (frame_callback == NULL)
		request_frame(false);
	buf->busy = true;
	wl_surface_attach(surface, buf->buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

static void
schedule_tick(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (config.second_mode == SECOND_SMOOTH)
		return;
	long seconds = config.second_mode == SECOND_TICK ?
	    1 : 60 - (now.tv_sec % 60);
	int64_t nanoseconds = (int64_t)seconds * 1000000000L - now.tv_nsec;
	struct timeval delay = {
		.tv_sec = nanoseconds / 1000000000L,
		.tv_usec = (nanoseconds % 1000000000L + 999) / 1000,
	};
	if (delay.tv_usec == 1000000) {
		delay.tv_sec++;
		delay.tv_usec = 0;
	}
	event_add(timer_event, &delay);
}

static void
timer_cb(__unused evutil_socket_t fd, __unused short events,
	    __unused void *data)
{
	if (config.second_mode == SECOND_TICK) {
		tick_pending = true;
		request_frame(true);
	} else {
		render();
	}
	schedule_tick();
}

static void
reload_cb(__unused evutil_socket_t signal, __unused short events,
	    __unused void *data)
{
	struct clock_config next;
	if (!load_config(&next)) {
		fprintf(stderr, "%s: reload failed; keeping current configuration\n",
		    PROG);
		return;
	}
	base_config = next;
	(void)refresh_effective_config();
	apply_timezone();
	int min_width, min_height;
	minimum_dimensions(&config, &min_width, &min_height);
	xdg_toplevel_set_min_size(xdg_toplevel, min_width, min_height);
	render();
	event_del(timer_event);
	schedule_tick();
	fprintf(stderr, "%s: configuration reloaded\n", PROG);
}

static void
wayland_cb(__unused evutil_socket_t fd, __unused short events,
	    __unused void *data)
{
	if (wl_display_dispatch(display) < 0)
		running = false;
}

static void
xdg_surface_configure(__unused void *data, struct xdg_surface *xs,
	    uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = true;
	render();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(__unused void *data,
	    __unused struct xdg_toplevel *toplevel, int32_t new_width,
	    int32_t new_height, struct wl_array *states)
{
	bool constrained = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED ||
		    *state == XDG_TOPLEVEL_STATE_FULLSCREEN ||
		    *state == XDG_TOPLEVEL_STATE_TILED_LEFT ||
		    *state == XDG_TOPLEVEL_STATE_TILED_RIGHT ||
		    *state == XDG_TOPLEVEL_STATE_TILED_TOP ||
		    *state == XDG_TOPLEVEL_STATE_TILED_BOTTOM)
			constrained = true;
	}

	if (new_width > 0 && new_height > 0 && config.aspect_ratio &&
	    !constrained) {
		double ratio = (double)config.width / config.height;
		if ((double)new_width / new_height > ratio)
			new_width = (int)lround(new_height * ratio);
		else
			new_height = (int)lround(new_width / ratio);
	}
	if (new_width > 0)
		width = new_width;
	if (new_height > 0)
		height = new_height;
}

static void
xdg_toplevel_close(__unused void *data,
	    __unused struct xdg_toplevel *toplevel)
{
	running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void
xdg_wm_base_ping(__unused void *data, struct xdg_wm_base *base,
	    uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
registry_global(__unused void *data, struct wl_registry *registry,
	    uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t bind_version = version < 4 ? version : 4;
		compositor = wl_registry_bind(registry, name,
		    &wl_compositor_interface, bind_version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name,
		    &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener,
		    NULL);
	}
}

static void
registry_remove(__unused void *data, __unused struct wl_registry *registry,
	    __unused uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void
usage(void)
{
	fprintf(stderr, "usage: cowclock [-c path] [-h]\n");
}

int
main(int argc, char **argv)
{
	const char *timezone = getenv("TZ");
	initial_timezone_set = timezone != NULL;
	if (initial_timezone_set)
		initial_timezone = strdup(timezone);

	int opt;
	while ((opt = getopt(argc, argv, "c:h")) != -1) {
		switch (opt) {
		case 'c':
			explicit_config = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}
	if (optind != argc) {
		usage();
		return 1;
	}
	if (!load_config(&base_config))
		return 1;
	config = base_config;
	apply_timezone();
	width = config.width;
	height = config.height;

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "%s: cannot connect to Wayland display\n", PROG);
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || wl_shm == NULL || xdg_wm_base == NULL) {
		fprintf(stderr, "%s: required Wayland globals unavailable\n", PROG);
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "Clock");
	xdg_toplevel_set_app_id(xdg_toplevel, PROG);
	int min_width, min_height;
	minimum_dimensions(&config, &min_width, &min_height);
	xdg_toplevel_set_min_size(xdg_toplevel, min_width, min_height);
	wl_surface_commit(surface);

	event_base = event_base_new();
	if (event_base == NULL) {
		fprintf(stderr, "%s: cannot initialise event loop\n", PROG);
		return 1;
	}
	connect_status();
	struct event *wayland_event = event_new(event_base,
	    wl_display_get_fd(display), EV_READ | EV_PERSIST, wayland_cb, NULL);
	timer_event = evtimer_new(event_base, timer_cb, NULL);
	struct event *reload_event = evsignal_new(event_base, SIGHUP, reload_cb,
	    NULL);
	if (wayland_event == NULL || timer_event == NULL ||
	    reload_event == NULL || event_add(wayland_event, NULL) < 0 ||
	    event_add(reload_event, NULL) < 0) {
		fprintf(stderr, "%s: cannot initialise event loop\n", PROG);
		return 1;
	}
	schedule_tick();
	while (running) {
		if (wl_display_flush(display) < 0 && errno != EAGAIN)
			break;
		if (event_base_loop(event_base, EVLOOP_ONCE) < 0)
			break;
	}

	event_free(reload_event);
	event_free(timer_event);
	event_free(wayland_event);
	if (status_bev != NULL)
		bufferevent_free(status_bev);
	if (frame_callback != NULL)
		wl_callback_destroy(frame_callback);
	event_base_free(event_base);
	cow_module_shm_buffers_destroy(buffers,
	    sizeof(buffers) / sizeof(buffers[0]));
	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	xdg_wm_base_destroy(xdg_wm_base);
	wl_shm_destroy(wl_shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	free(initial_timezone);
	return 0;
}
