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
#if !defined(COW_CONFIG_H)
#define COW_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "cow/colour.h"
#include "cow/vector.h"

struct cow_output;
struct cow_desk;

/*
 * Default config values.  Used both by cow_config_create() and by the
 * ternary fallbacks at sites where cow_config may be NULL (e.g. during
 * early startup before the config is parsed).  Keeping them here means
 * the two code paths cannot drift.
 */
#define COW_CFG_DEFAULT_BORDER 1
#define COW_CFG_DEFAULT_GAP 5
#define COW_CFG_DEFAULT_STEP 100
#define COW_CFG_DEFAULT_CORNER_LENGTH 20
#define COW_CFG_DEFAULT_ICON_SIZE 64
#define COW_CFG_DEFAULT_SNAP_DISTANCE 10
#define COW_CFG_DEFAULT_PAGE_COLS 1
#define COW_CFG_DEFAULT_PAGE_ROWS 1
#define COW_CFG_DEFAULT_PAGE_SCROLL_MS 200
#define COW_CFG_DEFAULT_EDGE_SCROLL_DELAY 300
#define COW_CFG_DEFAULT_SQUEEZE_WIDTH 300
#define COW_CFG_DEFAULT_SQUEEZE_MIN_WIDTH 80
/*
 * Key-repeat timing for bindings registered with `bind -r`.  Defaults
 * match the X11 / GNOME / KDE consensus (400ms initial dwell, then a
 * repeat every 40ms = 25 Hz).  Both are in milliseconds for
 * consistency with the other `_ms` knobs in the config.
 */
#define COW_CFG_DEFAULT_REPEAT_DELAY_MS 400
#define COW_CFG_DEFAULT_REPEAT_INTERVAL_MS 40

/*
 * Layer model (FvwmLayers-compatible).  Each window lives in a
 * numbered layer; higher number = stacks above lower.  Within a layer
 * the existing raise/lower (raise_seq) ordering applies.  Cross-layer
 * order is invariant: raising or lowering a window never moves it
 * past a layer boundary.
 */
#define COW_CFG_DEFAULT_LAYER 4
#define COW_CFG_MAX_LAYER 16

/*
 * When true (default) views migrated off a disappearing output keep
 * their desk number on the new output (if such a desk exists there).
 * When false they land on the new output's current desk regardless of
 * where they were before.
 */
#define COW_CFG_DEFAULT_PRESERVE_DESK_ON_OUTPUT_LOSS true

/*
 * Border style enum (carried from original cow).
 */
enum cow_border_style {
	COW_BORDER_STYLE_NONE = -1,
	COW_BORDER_STYLE_MWM  = 0,
	COW_BORDER_STYLE_FVWM = 1,
};

/*
 * Window placement mode.
 */
enum cow_placement_mode {
	COW_PLACEMENT_CASCADE,
	COW_PLACEMENT_UNDERMOUSE,
	COW_PLACEMENT_CENTRE,
};

/*
 * Per-edge border colors.
 */
struct cow_border_colour {
	float n[4], s[4], e[4], w[4];
	float nw[4], ne[4], sw[4], se[4];
	bool  has_corners;
	bool  has_nw, has_ne, has_sw, has_se;
};

/*
 * A single parsed keybinding action string from the config.
 *
 * repeatable: set by `bind -r`.  While the triggering key is held
 * the bound action re-fires every cow_config->repeat_interval_ms
 * after an initial cow_config->repeat_delay_ms dwell.  When false
 * the binding fires exactly once per press.
 */
struct cow_binding_entry {
	struct wl_list link;
	char	      *key_string; /* e.g. "L+Return", "LS+q" */
	char *action_string; /* e.g. "action-terminal", "quit", "view-quit" */
	bool  repeatable;
};

/*
 * A single parsed mouse binding from the config.
 */
struct cow_mouse_binding_entry {
	struct wl_list link;
	char	      *key_string;    /* e.g. "L+left" */
	char	      *action_string; /* e.g. "mode-enter-move" */
};

/*
 * Decoration regions a click can land in.
 */
enum cow_dec_region {
	COW_DEC_REGION_NONE = 0,
	COW_DEC_REGION_N,
	COW_DEC_REGION_S,
	COW_DEC_REGION_E,
	COW_DEC_REGION_W,
	COW_DEC_REGION_NE,
	COW_DEC_REGION_NW,
	COW_DEC_REGION_SE,
	COW_DEC_REGION_SW,
	COW_DEC_REGION_TITLEBAR,
	COW_DEC_REGION_COUNT,
};

struct cow_dec_binding_entry {
	struct wl_list	    link;
	enum cow_dec_region region;
	int		    clicks;
	char		   *action_string; /* full cmd-string body */
};

/*
 * Button action enum.
 */
enum cow_button_action {
	COW_BUTTON_ACTION_CLOSE,
	COW_BUTTON_ACTION_MAXIMIZE,
	COW_BUTTON_ACTION_ICONIFY,
	COW_BUTTON_ACTION_NONE,
};

/*
 * Squeezed titlebar (CTWM SqueezeTitle).  When enabled, the titlebar
 * is painted as a rectangle strictly narrower than the window frame;
 * the remainder of the titlebar strip is transparent and passes
 * clicks through to whatever is underneath.
 *
 *   NONE    full-width titlebar (default; same as unsqueezed)
 *   NATURAL width follows the title-text extent + button run, clamped
 *           to >= min_width
 *   FIXED   width is taken from cow_titlebar_squeeze.width
 */
enum cow_squeeze_mode {
	COW_SQUEEZE_NONE = 0,
	COW_SQUEEZE_NATURAL,
	COW_SQUEEZE_FIXED,
};

enum cow_squeeze_just {
	COW_JUST_LEFT = 0,
	COW_JUST_CENTRE,
	COW_JUST_RIGHT,
};

struct cow_titlebar_squeeze {
	enum cow_squeeze_mode mode;
	enum cow_squeeze_just justify;
	int		      width;	 /* fixed mode */
	int		      offset;	 /* signed; nudge after justify */
	int		      min_width; /* floor for natural mode */
};

/*
 * Parse a squeeze mode token ("none"/"natural"/"fixed") into 'out'.
 * Returns true on success; false (and leaves 'out' untouched) on any
 * other input.
 */
bool cow_parse_squeeze_mode(const char *, enum cow_squeeze_mode *);

/*
 * Parse a squeeze justify token ("left"/"centre"/"center"/"right")
 * into 'out'.  Returns true on success; false (and leaves 'out'
 * untouched) on any other input.
 */
bool cow_parse_squeeze_just(const char *, enum cow_squeeze_just *);

/*
 * Titlebar button config.
 */
struct cow_titlebar_button_config {
	struct cow_vector_button vector;
	enum cow_button_action	 action;
};

#define COW_TITLEBAR_NUM_BUTTONS 3

/*
 * Container tab strip styling.
 * Controls the colours used for tab labels inside a container titlebar.
 * Defaults are set in cow_config_create(); all four values can be
 * overridden via 'set container.tab.*' commands.
 */
struct cow_container_config {
	float tab_active[4];	  /* background of the active tab label   */
	float tab_inactive[4];	  /* background of inactive tab labels    */
	float tab_fg_active[4];	  /* foreground (text) of the active tab  */
	float tab_fg_inactive[4]; /* foreground (text) of inactive tabs   */

	/* Set to true when the user explicitly configures the value.
	 * When false, titlebar.c falls back to the titlebar's base/dk/fg. */
	bool has_tab_active;
	bool has_tab_inactive;
	bool has_tab_fg_active;
	bool has_tab_fg_inactive;

	/* Whether inactive tabs are drawn with a sunken bevel.
	 * Default false (flat); set true for the classic depressed look. */
	bool tab_sunken_inactive;
};

/*
 * Titlebar configuration.
 */
struct cow_titlebar_config {
	bool				  enabled;
	struct cow_titlebar_button_config buttons[COW_TITLEBAR_NUM_BUTTONS];
	float				  active_colour[4];
	float				  inactive_colour[4];
	float				  fg_active[4];
	float				  fg_inactive[4];
	char				 *font_name;
	/* Explicit height override; 0 means derive from font. */
	int height;

	/* Squeezed titlebar (CTWM SqueezeTitle). */
	struct cow_titlebar_squeeze squeeze;
};

/*
 * The portable configuration struct for the river-based cow.
 *
 * Parsed from the line-based cow.conf format.  Only contains
 * settings that are relevant to the river window manager (no
 * compositor-specific settings like output backgrounds, input
 * device configs, or keyboard keymaps).
 */
struct cow_config {
	/* UI settings */
	int		      border;
	int		      gap;
	int		      step;
	int		      corner_length;
	bool		      handles; /* true = draw handle marks on borders */
	enum cow_border_style border_style;
	enum cow_placement_mode placement;

	/* Desktop configuration mode */
	enum {
		COW_DESKTOP_GLOBAL,
		COW_DESKTOP_PER_OUTPUT,
		COW_DESKTOP_SHARED,
	} desktop_config;

	/* Virtual page grid: each desk has page_cols × page_rows pages.
	 * Default 1×1 preserves the existing single-page behaviour. */
	int page_cols; /* columns (horizontal) */
	int page_rows; /* rows (vertical) */

	/* Smooth page scroll animation.
	 * When true, page switches animate over page_scroll_ms milliseconds.
	 * Default false (instant cut, matching desk-switch behaviour). */
	bool page_scroll_animate;
	int  page_scroll_ms; /* animation duration in ms, default 200 */

	/* Edge scroll: trigger a page switch when the pointer dwells at a
	 * screen edge for edge_scroll_delay_ms milliseconds.
	 * Default false (disabled). */
	bool edge_scroll;
	int  edge_scroll_delay_ms; /* delay before scroll fires, default 300 */

	/*
	 * Key-repeat timing for bindings declared `bind -r`.
	 * repeat_delay_ms: how long the user must hold the key before
	 *   the action starts repeating after its first fire.
	 * repeat_interval_ms: gap between consecutive repeats once
	 *   repetition has started.
	 */
	int repeat_delay_ms;
	int repeat_interval_ms;

	/* Raise on focus: raise window when focused (default: on) */
	bool raise_on_focus;

	/* Preserve desk number when migrating views to a different
	 * output (e.g. when a monitor is unplugged). */
	bool preserve_desk_on_output_loss;

	/*
	 * Layer assigned to newly mapped windows.  Existing windows are
	 * unaffected by changes to this value; rules can set a different
	 * layer at map time via `winops -L`.
	 */
	uint8_t default_layer;

	/*
	 * Upper bound for layer numbers.  Assignments above this cap are
	 * clamped.  Lowering max_layer below an existing window's layer
	 * does NOT silently rewrite that window; callers can re-run
	 * `winops -L =top` to bring it down.
	 */
	uint8_t max_layer;

	/* Colors */
	struct cow_border_colour border_active;
	struct cow_border_colour border_inactive;

	/* Snap attraction distance (0 = disabled) */
	int snap_distance;

	/* Focus mode */
	enum {
		COW_FOCUS_MODE_CLICK,
		COW_FOCUS_MODE_SLOPPY,
	} focus_mode;

	/* Font */
	char *font_name;

	/* Icon size for iconified (minimized) windows */
	int icon_size;

	/* Titlebar */
	struct cow_titlebar_config titlebar;

	/* Container tab strip */
	struct cow_container_config container;

	/* Reserved area: pixels to subtract from usable area for a bar/panel.
	 * Fullscreen ignores this; maximize/snap/cascade respect it. */
	struct {
		int top;
		int bottom;
		int left;
		int right;
	} reserved;

	/* Output background colour (from outputs config) */
	float output_bg_colour[4];
	bool  has_output_bg_colour;

	/* Parsed binding lists */
	struct wl_list keyboard_bindings;   /* cow_binding_entry.link */
	struct wl_list mouse_bindings;	    /* cow_mouse_binding_entry.link */
	struct wl_list decoration_bindings; /* cow_dec_binding_entry.link */
	struct wl_list actions;		    /* cow_action_entry.link */
};

extern struct cow_config *cow_config;

/*
 * Parse a vector button definition string.
 * Format: "N x1xy1@c1 x2xy2@c2 ..."
 */
bool cow_parse_vector_button_string(struct cow_vector_button *, const char *);

/*
 * Allocate and initialize a config with defaults.
 */
struct cow_config *cow_config_create(void);

/*
 * Free a config and all its lists.
 */
void cow_config_destroy(struct cow_config *);

/*
 * Resolve the default config file path.
 * Checks: $COW_CONFIG, $XDG_CONFIG_HOME/cow/cow.conf,
 * ~/.config/cow/cow.conf, /etc/cow/cow.conf.
 */
const char *cow_config_default_path(void);

/*
 * Find an action entry by name. Returns the command string, or NULL.
 */
const char *cow_config_find_action(struct cow_config *, const char *);

/*
 * Parse a region name ("N", "south", "ne", "titlebar", ...) into a
 * cow_dec_region value.  Case-insensitive.  Returns COW_DEC_REGION_NONE
 * on a bad name.
 */
enum cow_dec_region cow_dec_region_parse(const char *);

const char *cow_dec_region_name(enum cow_dec_region);

/*
 * Look up a decoration-click binding for the given region.  Caller
 * should treat the returned pointer as borrowed: it's owned by the
 * cow_config binding list.
 */
const char *cow_dec_binding_lookup(enum cow_dec_region, int clicks);

struct cow_view;
bool cow_dec_binding_fire(struct cow_view *, enum cow_dec_region, int clicks);

/*
 * Populate the current cow_config->decoration_bindings list with
 * the built-in defaults (resize for each edge/corner; move +
 * double-click-shade for the titlebar).  Call once after
 * cow_config_create so user `bind-decoration` lines in the config
 * naturally override these defaults via the same-pair replacement
 * rule in cmd-bind-decoration.
 */
void cow_decoration_register_default_bindings(void);

#endif
