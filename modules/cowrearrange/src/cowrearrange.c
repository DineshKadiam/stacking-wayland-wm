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
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "cow/module.h"

#define MIN_WIN_W 50
#define MIN_WIN_H 50

enum mode {
	MODE_TILE,
	MODE_CASCADE,
};

struct opts {
	enum mode   mode;
	const char *output_name;     /* NULL = current */
	int	    desk;	     /* -1 = current */
	int	    tile_rows;	     /* 0 = auto */
	int	    tile_cols;	     /* 0 = auto */
	int	    cascade_dx;	     /* -1 = auto */
	int	    cascade_dy;	     /* -1 = auto */
	bool	    resize;	     /* honour -r / -R */
	bool	    resize_explicit; /* user passed -r or -R */
	bool	    include_special; /* -i: include max/shaded */
	/*
	 * Exclusion selectors -- each follows cow's window selector
	 * syntax: %app_id, /title-substring/, #window-id.
	 * Repeatable on the command line via multiple --exclude flags;
	 * a window matching ANY entry is dropped from the candidate
	 * set before the layout is computed.
	 */
	const char **exclude;
	int	     n_exclude;
	int	     exclude_cap;
};

/* A window we plan to relocate / resize. */
struct win {
	char *id;
	int   border_width;
	int   title_thickness;
	int   content_w;
	int   content_h;
	int   raise_seq; /* tie-breaker for stable order */
};

static char *
ipc_request(const char *cmd)
{
	return cow_module_cmd_request("cowrearrange", cmd, true);
}

/*
 * Parse a JSON response and extract the "data" payload as a
 * detached cJSON node the caller owns.  Returns NULL on failure,
 * with cause logged to stderr.
 */
static cJSON *
parse_response(const char *resp, const char *what)
{
	cJSON *root = cJSON_Parse(resp);
	if (root == NULL) {
		fprintf(stderr, "cowrearrange: %s: bad JSON response\n", what);
		return NULL;
	}
	cJSON *ok = cJSON_GetObjectItem(root, "ok");
	if (cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
		cJSON *err = cJSON_GetObjectItem(root, "error");
		fprintf(stderr, "cowrearrange: %s: %s\n", what,
		    cJSON_IsString(err) ? err->valuestring : "(unknown)");
		cJSON_Delete(root);
		return NULL;
	}
	cJSON *data = cJSON_GetObjectItem(root, "data");
	cJSON *out  = NULL;
	if (data != NULL)
		out = cJSON_Duplicate(data, 1);
	cJSON_Delete(root);
	return out;
}

static void
usage(FILE *fp)
{
	fprintf(fp,
	    "usage: cowrearrange tile    [-o output] [-d desk] [--rows N]\n"
	    "                            [--cols N] [-r|-R] [-i]\n"
	    "                            [-x SELECTOR ...]\n"
	    "       cowrearrange cascade [-o output] [-d desk] [--dx N]\n"
	    "                            [--dy N] [-r|-R] [-i]\n"
	    "                            [-x SELECTOR ...]\n"
	    "\n"
	    "  -o output       target output by name (default: current)\n"
	    "  -d desk         target desk index (default: current)\n"
	    "  --rows N        tile: fix the number of rows (default: auto)\n"
	    "  --cols N        tile: fix the number of cols (default: auto)\n"
	    "  --dx N          cascade: horizontal pixel step (default: auto)\n"
	    "  --dy N          cascade: vertical pixel step (default: auto)\n"
	    "  -r              resize windows to fit (tile default)\n"
	    "  -R              preserve sizes; place only (cascade default)\n"
	    "  -i              include maximised / shaded windows\n"
	    "  -x, --exclude   exclude windows matching SELECTOR (repeatable);\n"
	    "                  SELECTOR is %%app_id, /title-substring/, or #id\n");
}

static int
parse_int_arg(const char *who, const char *val, int *out)
{
	char *end;
	long  v = strtol(val, &end, 10);
	if (*val == '\0' || *end != '\0' || v < 0 || v > 65535) {
		fprintf(stderr, "cowrearrange: %s: bad integer '%s'\n", who,
		    val);
		return -1;
	}
	*out = (int)v;
	return 0;
}

static int
parse_opts(int argc, char *argv[], struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->desk	      = -1;
	o->cascade_dx = -1;
	o->cascade_dy = -1;
	/* resize default is set after the mode is known. */

	if (argc < 2) {
		usage(stderr);
		return -1;
	}
	if (strcmp(argv[1], "tile") == 0)
		o->mode = MODE_TILE;
	else if (strcmp(argv[1], "cascade") == 0)
		o->mode = MODE_CASCADE;
	else {
		usage(stderr);
		return -1;
	}

	/*
	 * Mode-specific resize defaults.  Tile fits cells to the
	 * workarea by default; cascade preserves the user's window
	 * sizes by default (matching FvwmRearrange's -cascade
	 * behaviour where -resize is opt-in).
	 */
	o->resize = (o->mode == MODE_TILE);

	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-o") == 0 && i + 1 < argc) {
			o->output_name = argv[++i];
		} else if (strcmp(a, "-d") == 0 && i + 1 < argc) {
			if (parse_int_arg("-d", argv[++i], &o->desk) < 0)
				return -1;
		} else if (strcmp(a, "--rows") == 0 && i + 1 < argc) {
			if (parse_int_arg("--rows", argv[++i], &o->tile_rows) <
			    0)
				return -1;
		} else if (strcmp(a, "--cols") == 0 && i + 1 < argc) {
			if (parse_int_arg("--cols", argv[++i], &o->tile_cols) <
			    0)
				return -1;
		} else if (strcmp(a, "--dx") == 0 && i + 1 < argc) {
			if (parse_int_arg("--dx", argv[++i], &o->cascade_dx) <
			    0)
				return -1;
		} else if (strcmp(a, "--dy") == 0 && i + 1 < argc) {
			if (parse_int_arg("--dy", argv[++i], &o->cascade_dy) <
			    0)
				return -1;
		} else if (strcmp(a, "-r") == 0) {
			o->resize	   = true;
			o->resize_explicit = true;
		} else if (strcmp(a, "-R") == 0) {
			o->resize	   = false;
			o->resize_explicit = true;
		} else if (strcmp(a, "-i") == 0) {
			o->include_special = true;
		} else if (strcmp(a, "--exclude") == 0 ||
		    strcmp(a, "-x") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr,
				    "cowrearrange: %s: missing SELECTOR\n", a);
				return -1;
			}
			const char *spec = argv[++i];
			if (spec[0] != '%' && spec[0] != '#' &&
			    spec[0] != '/') {
				fprintf(stderr,
				    "cowrearrange: %s: bad selector '%s' "
				    "(expected %%app, /title/, or #id)\n",
				    a, spec);
				return -1;
			}
			if (o->n_exclude == o->exclude_cap) {
				int ncap = o->exclude_cap ? o->exclude_cap * 2 :
							    4;
				const char **nb = realloc(o->exclude,
				    (size_t)ncap * sizeof(*nb));
				if (nb == NULL) {
					fprintf(stderr,
					    "cowrearrange: out of memory\n");
					return -1;
				}
				o->exclude     = nb;
				o->exclude_cap = ncap;
			}
			o->exclude[o->n_exclude++] = spec;
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			usage(stdout);
			exit(0);
		} else {
			fprintf(stderr, "cowrearrange: unknown argument: %s\n",
			    a);
			usage(stderr);
			return -1;
		}
	}
	return 0;
}

static const char *
js_string(cJSON *obj, const char *key)
{
	cJSON *v = cJSON_GetObjectItem(obj, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int
js_int(cJSON *obj, const char *key, int dflt)
{
	cJSON *v = cJSON_GetObjectItem(obj, key);
	return cJSON_IsNumber(v) ? (int)v->valuedouble : dflt;
}

static bool
js_bool(cJSON *obj, const char *key, bool dflt)
{
	cJSON *v = cJSON_GetObjectItem(obj, key);
	if (cJSON_IsBool(v))
		return cJSON_IsTrue(v);
	return dflt;
}

struct workarea {
	int x, y, w, h;
};

/*
 * Resolve the workarea for the named (or current) output.  When
 * name is NULL, pick the one whose usable rect contains the most
 * recent focus -- approximated here as "the first output that
 * matches the focused window's output_name from show windows", with
 * a fallback to the first output in the list.
 */
static int
resolve_workarea(cJSON *outputs, const char *want_name, char *out_name,
    size_t out_name_sz, struct workarea *wa)
{
	if (!cJSON_IsArray(outputs)) {
		fprintf(stderr, "cowrearrange: show output: not an array\n");
		return -1;
	}
	cJSON *o;
	cJSON_ArrayForEach(o, outputs)
	{
		const char *n = js_string(o, "name");
		if (n == NULL)
			continue;
		if (want_name != NULL && strcmp(n, want_name) != 0)
			continue;
		cJSON *ua = cJSON_GetObjectItem(o, "usable");
		if (!cJSON_IsObject(ua))
			continue;
		wa->x = js_int(ua, "x", 0);
		wa->y = js_int(ua, "y", 0);
		wa->w = js_int(ua, "width", 0);
		wa->h = js_int(ua, "height", 0);
		strncpy(out_name, n, out_name_sz - 1);
		out_name[out_name_sz - 1] = '\0';
		return 0;
	}
	if (want_name != NULL) {
		fprintf(stderr, "cowrearrange: unknown output: %s\n",
		    want_name);
		return -1;
	}
	/* No focus heuristic available; take first. */
	o = cJSON_GetArrayItem(outputs, 0);
	if (o == NULL) {
		fprintf(stderr, "cowrearrange: no outputs\n");
		return -1;
	}
	const char *n = js_string(o, "name");
	if (n != NULL) {
		strncpy(out_name, n, out_name_sz - 1);
		out_name[out_name_sz - 1] = '\0';
	}
	cJSON *ua = cJSON_GetObjectItem(o, "usable");
	wa->x	  = js_int(ua, "x", 0);
	wa->y	  = js_int(ua, "y", 0);
	wa->w	  = js_int(ua, "width", 0);
	wa->h	  = js_int(ua, "height", 0);
	return 0;
}

/*
 * Test whether 'w' matches the window selector 'spec'.
 *
 * Returns true on match.  An unrecognised selector returns false
 * (it can't have matched anything).
 */
static bool
window_matches_spec(cJSON *w, const char *spec)
{
	if (spec == NULL || spec[0] == '\0')
		return false;
	switch (spec[0]) {
	case '%': {
		const char *want = spec + 1;
		const char *aid	 = js_string(w, "app_id");
		return aid != NULL && strcmp(aid, want) == 0;
	}
	case '/': {
		/* Strip optional trailing '/' so /foo/ and /foo behave
		 * identically -- matches cow's cmd-ctx semantics. */
		const char *body  = spec + 1;
		size_t	    len	  = strlen(body);
		const char *title = js_string(w, "title");
		if (title == NULL)
			return false;
		if (len > 0 && body[len - 1] == '/') {
			/* Compare against the substring sans trailing /. */
			char tmp[256];
			if (len - 1 >= sizeof(tmp))
				return false;
			memcpy(tmp, body, len - 1);
			tmp[len - 1] = '\0';
			return strstr(title, tmp) != NULL;
		}
		return strstr(title, body) != NULL;
	}
	case '#': {
		const char *want = spec + 1;
		const char *id	 = js_string(w, "id");
		return id != NULL && strcmp(id, want) == 0;
	}
	default:
		return false;
	}
}

/*
 * Return true if any of opts->exclude matches w.  A window is
 * dropped from the candidate set on first match.
 */
static bool
window_excluded(cJSON *w, const struct opts *opts)
{
	for (int i = 0; i < opts->n_exclude; i++) {
		if (window_matches_spec(w, opts->exclude[i]))
			return true;
	}
	return false;
}

/*
 * Walk the show windows array, filter to the candidate set, dedupe
 * container members (keep the active member), and return a malloc'd
 * array.  *n_out gets the count.  Caller must free each id and the
 * array.
 */
static struct win *
collect_candidates(cJSON *windows, const struct opts *opts, int target_desk,
    const char *target_output, int *n_out)
{
	*n_out = 0;
	if (!cJSON_IsArray(windows)) {
		fprintf(stderr, "cowrearrange: show windows: not an array\n");
		return NULL;
	}

	int	    cap = 16;
	int	    n	= 0;
	struct win *arr = calloc(cap, sizeof(*arr));
	if (arr == NULL)
		return NULL;

	/*
	 * To dedupe container members we collect container_ids we've
	 * already accepted (active-member-only).  Small ad-hoc list;
	 * O(n) lookup is fine for desk-sized N.
	 */
	int    seen_cap	  = 16;
	int    seen_n	  = 0;
	char **seen_ctrid = calloc(seen_cap, sizeof(*seen_ctrid));

	cJSON *w;
	cJSON_ArrayForEach(w, windows)
	{
		int desk = js_int(w, "desk", -1);
		if (desk != target_desk)
			continue;
		const char *outn = js_string(w, "output");
		if (outn == NULL || strcmp(outn, target_output) != 0)
			continue;
		if (js_bool(w, "iconified", false))
			continue;
		if (!opts->include_special &&
		    (js_bool(w, "maximized", false) ||
			js_bool(w, "shaded", false)))
			continue;

		/*
		 * User-supplied --exclude selectors.  A window matching
		 * any selector is dropped before container dedupe so a
		 * container member can be excluded by app_id / title
		 * without the rest of the container's members being
		 * pulled in as a substitute.
		 */
		if (opts->n_exclude > 0 && window_excluded(w, opts))
			continue;

		/* Container dedupe: skip any view that is NOT the
		 * active member of its container.  Multiple views with
		 * the same container_id collapse to one entry. */
		const char *cid = js_string(w, "container_id");
		if (cid != NULL) {
			bool active = js_bool(w, "container_active", false);
			if (!active)
				continue;
			bool dup = false;
			for (int i = 0; i < seen_n; i++) {
				if (strcmp(seen_ctrid[i], cid) == 0) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			if (seen_n == seen_cap) {
				seen_cap *= 2;
				char **nb = realloc(seen_ctrid,
				    seen_cap * sizeof(*nb));
				if (nb == NULL)
					goto oom;
				seen_ctrid = nb;
			}
			seen_ctrid[seen_n++] = strdup(cid);
		}

		const char *id = js_string(w, "id");
		if (id == NULL || *id == '\0')
			continue;

		if (n == cap) {
			cap *= 2;
			struct win *nb = realloc(arr, cap * sizeof(*arr));
			if (nb == NULL)
				goto oom;
			memset(nb + n, 0, (cap - n) * sizeof(*nb));
			arr = nb;
		}
		arr[n].id	       = strdup(id);
		arr[n].border_width    = js_int(w, "border_width", 0);
		arr[n].title_thickness = js_int(w, "title_thickness", 0);
		arr[n].content_w       = js_int(w, "width", MIN_WIN_W);
		arr[n].content_h       = js_int(w, "height", MIN_WIN_H);
		arr[n].raise_seq       = js_int(w, "raise_seq", 0);
		n++;
	}

	for (int i = 0; i < seen_n; i++)
		free(seen_ctrid[i]);
	free(seen_ctrid);
	*n_out = n;
	return arr;

oom:
	for (int i = 0; i < n; i++)
		free(arr[i].id);
	free(arr);
	for (int i = 0; i < seen_n; i++)
		free(seen_ctrid[i]);
	free(seen_ctrid);
	*n_out = 0;
	return NULL;
}

static int
cmp_raise_seq(const void *a, const void *b)
{
	const struct win *wa = a;
	const struct win *wb = b;
	if (wa->raise_seq < wb->raise_seq)
		return -1;
	if (wa->raise_seq > wb->raise_seq)
		return 1;
	return 0;
}

struct placed {
	const char *id;
	int	    content_x, content_y;
	int	    content_w, content_h;
};

static void
layout_tile(struct win *wins, int n, const struct workarea *wa,
    const struct opts *opts, struct placed *out)
{
	int cols = opts->tile_cols;
	int rows = opts->tile_rows;
	if (cols <= 0) {
		if (rows > 0)
			cols = (n + rows - 1) / rows;
		else
			cols = (int)ceil(sqrt((double)n));
	}
	if (rows <= 0)
		rows = (n + cols - 1) / cols;

	int cell_h     = wa->h / rows;
	int last_row_n = n - (rows - 1) * cols;
	if (last_row_n <= 0)
		last_row_n = cols;

	for (int i = 0; i < n; i++) {
		int r		   = i / cols;
		int c		   = i % cols;
		int cells_this_row = (r == rows - 1 && last_row_n != cols) ?
		    last_row_n :
		    cols;
		int cell_w	   = wa->w / cells_this_row;
		int frame_x	   = wa->x + c * cell_w;
		int frame_y	   = wa->y + r * cell_h;
		int bw		   = wins[i].border_width;
		int tt		   = wins[i].title_thickness;
		int cw		   = wins[i].content_w;
		int ch		   = wins[i].content_h;
		if (opts->resize) {
			cw = cell_w - 2 * bw;
			ch = cell_h - 2 * bw - tt;
			if (cw < MIN_WIN_W)
				cw = MIN_WIN_W;
			if (ch < MIN_WIN_H)
				ch = MIN_WIN_H;
		}
		out[i].id	 = wins[i].id;
		out[i].content_x = frame_x + bw;
		out[i].content_y = frame_y + bw + tt;
		out[i].content_w = cw;
		out[i].content_h = ch;
	}
}

static void
layout_cascade(struct win *wins, int n, const struct workarea *wa,
    const struct opts *opts, struct placed *out)
{
	/*
	 * Default step taken from the first window's title bar +
	 * border so each titlebar peeks out below the one above it.
	 * The user can override either axis with --dx / --dy.
	 */
	int step_default = (n > 0) ?
	    wins[0].title_thickness + wins[0].border_width :
	    24;
	if (step_default < 8)
		step_default = 24;
	int dx = (opts->cascade_dx >= 0) ? opts->cascade_dx : step_default;
	int dy = (opts->cascade_dy >= 0) ? opts->cascade_dy : step_default;

	int target_w = 0;
	int target_h = 0;
	if (opts->resize && n > 0) {
		/* Size so the last cascaded window's bottom-right edge
		 * lines up with the workarea bottom-right.  All windows
		 * get the same size in resize mode. */
		int bw	 = wins[0].border_width;
		int tt	 = wins[0].title_thickness;
		target_w = wa->w - (n - 1) * dx - 2 * bw;
		target_h = wa->h - (n - 1) * dy - 2 * bw - tt;
		if (target_w < MIN_WIN_W)
			target_w = MIN_WIN_W;
		if (target_h < MIN_WIN_H)
			target_h = MIN_WIN_H;
	}

	for (int i = 0; i < n; i++) {
		int bw		 = wins[i].border_width;
		int tt		 = wins[i].title_thickness;
		int frame_x	 = wa->x + i * dx;
		int frame_y	 = wa->y + i * dy;
		int cw		 = opts->resize ? target_w : wins[i].content_w;
		int ch		 = opts->resize ? target_h : wins[i].content_h;
		out[i].id	 = wins[i].id;
		out[i].content_x = frame_x + bw;
		out[i].content_y = frame_y + bw + tt;
		out[i].content_w = cw;
		out[i].content_h = ch;
	}
}

/*
 * Build one batched command-string of the form
 *   window-move -t :::#A -x X1 -y Y1 ; window-resize -t :::#A -w W1 -h H1 ; ...
 * The IPC buffer in cow has a generous limit; for desk-sized N this
 * fits comfortably.  Returns a malloc'd string, NULL on failure.
 */
static char *
build_cmd(const struct placed *plan, int n)
{
	size_t cap = 256;
	size_t len = 0;
	char  *buf = malloc(cap);
	if (buf == NULL)
		return NULL;
	buf[0] = '\0';

	for (int i = 0; i < n; i++) {
		char chunk[512];
		int nm = snprintf(chunk, sizeof(chunk),
		    "%swindow-move -t \"#%s\" -x %d -y %d ; "
		    "window-resize -t \"#%s\" -w %d -h %d",
		    (i == 0) ? "" : " ; ", plan[i].id, plan[i].content_x,
		    plan[i].content_y, plan[i].id, plan[i].content_w,
		    plan[i].content_h);
		if (nm < 0 || (size_t)nm >= sizeof(chunk)) {
			free(buf);
			return NULL;
		}
		if (len + (size_t)nm + 2 > cap) {
			while (len + (size_t)nm + 2 > cap)
				cap *= 2;
			char *nb = realloc(buf, cap);
			if (nb == NULL) {
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		memcpy(buf + len, chunk, (size_t)nm);
		len += (size_t)nm;
		buf[len] = '\0';
	}
	return buf;
}

int
main(int argc, char *argv[])
{
	struct opts opts;
	if (parse_opts(argc, argv, &opts) < 0)
		return 1;

	/*
	 * cow's IPC server closes the connection after each command,
	 * so ipc_request opens / closes its own socket per call.  No
	 * persistent fd to manage here.
	 */
	char	      *resp_windows = ipc_request("show windows\n");
	char	      *resp_output  = ipc_request("show output\n");
	char	      *resp_desk    = ipc_request("show desk\n");
	int	       rc	    = 1;
	cJSON	      *jw = NULL, *jo = NULL, *jd = NULL;
	struct win    *wins = NULL;
	struct placed *plan = NULL;

	int n = 0;

	if (resp_windows == NULL || resp_output == NULL || resp_desk == NULL)
		goto out;

	jw = parse_response(resp_windows, "show windows");
	jo = parse_response(resp_output, "show output");
	jd = parse_response(resp_desk, "show desk");
	if (jw == NULL || jo == NULL)
		goto out;

	char		out_name[64];
	struct workarea wa;
	if (resolve_workarea(jo, opts.output_name, out_name, sizeof(out_name),
		&wa) < 0)
		goto out;

	int target_desk = opts.desk;
	if (target_desk < 0) {
		if (jd != NULL)
			target_desk = js_int(jd, "current", 0);
		else
			target_desk = 0;
	}

	int got = 0;
	wins	= collect_candidates(jw, &opts, target_desk, out_name,
	       &got);
	n	= got;

	if (wins == NULL && n > 0)
		goto out;
	if (n == 0) {
		fprintf(stderr,
		    "cowrearrange: no candidate windows on %s desk %d\n",
		    out_name, target_desk);
		rc = 0;
		goto out;
	}
	if (wins == NULL && n > 0)
		goto out;
	if (n == 0) {
		fprintf(stderr,
		    "cowrearrange: no candidate windows on %s desk %d\n",
		    out_name, target_desk);
		rc = 0;
		goto out;
	}

	/* Stable order: bottom-of-stack first so the visually-topmost
	 * window ends up at the last cell / cascade slot, which is
	 * usually what the user expects.
	 */
	qsort(wins, (size_t)n, sizeof(*wins), cmp_raise_seq);

	plan = calloc((size_t)n, sizeof(*plan));
	if (plan == NULL)
		goto out;

	if (opts.mode == MODE_TILE)
		layout_tile(wins, n, &wa, &opts, plan);
	else
		layout_cascade(wins, n, &wa, &opts, plan);

	char *cmd = build_cmd(plan, n);
	if (cmd == NULL) {
		fprintf(stderr,
		    "cowrearrange: failed to build batched command\n");
		goto out;
	}
	char *resp_apply = ipc_request(cmd);
	free(cmd);

	if (resp_apply == NULL)
		goto out;
	/* FIXME: we're being lazy here by not using cJSON. */
	if (strstr(resp_apply, "\"ok\":false") != NULL) {
		fprintf(stderr, "cowrearrange: cow reported failure: %s",
		    resp_apply);
		free(resp_apply);
		goto out;
	}
	free(resp_apply);
	rc = 0;

out:
	if (wins != NULL) {
		for (int i = 0; i < n; i++)
			free(wins[i].id);
		free(wins);
	}
	free(plan);
	if (jw != NULL)
		cJSON_Delete(jw);
	if (jo != NULL)
		cJSON_Delete(jo);
	if (jd != NULL)
		cJSON_Delete(jd);
	free(resp_windows);
	free(resp_output);
	free(resp_desk);
	return rc;
}
