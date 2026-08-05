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
#if !defined(COW_MENU_H)
#define COW_MENU_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "cow/colour.h"

struct river_shell_surface_v1;
struct river_node_v1;
struct cow_shm_buffer;
struct cow_view;

enum cow_menu_selection_relief {
	COW_MENU_SELECTION_RELIEF_NONE,
	COW_MENU_SELECTION_RELIEF_RAISED,
};

struct cow_menu_style {
	float bg[4];	    /* background colour */
	float fg[4];	    /* item text colour */
	float title_bg[4];  /* title item background */
	float title_fg[4];  /* title item text colour */
	float separator[4]; /* separator line colour */
	float border[4];    /* outer border colour */
	struct cow_gradient bg_gradient;
	struct cow_gradient title_bg_gradient;
	int   border_width; /* outer border thickness (px) */
	int   padding_x;    /* horizontal padding within items (px) */
	int   padding_y;    /* vertical padding above/below text (px) */
	int   title_underlines;
	bool  automatic_hotkeys;
	enum cow_menu_selection_relief selection_relief;
};

enum cow_menu_style_field {
	COW_MENU_STYLE_BG		 = 1u << 0,
	COW_MENU_STYLE_FG		 = 1u << 1,
	COW_MENU_STYLE_TITLE_BG		 = 1u << 2,
	COW_MENU_STYLE_TITLE_FG		 = 1u << 3,
	COW_MENU_STYLE_SEPARATOR	 = 1u << 4,
	COW_MENU_STYLE_BG_GRADIENT	 = 1u << 5,
	COW_MENU_STYLE_TITLE_BG_GRADIENT = 1u << 6,
	COW_MENU_STYLE_BORDER_WIDTH	 = 1u << 7,
	COW_MENU_STYLE_PADDING_X	 = 1u << 8,
	COW_MENU_STYLE_PADDING_Y	 = 1u << 9,
	COW_MENU_STYLE_TITLE_UNDERLINES	 = 1u << 10,
	COW_MENU_STYLE_AUTOMATIC_HOTKEYS = 1u << 11,
	COW_MENU_STYLE_SELECTION_RELIEF	 = 1u << 12,
	COW_MENU_STYLE_BORDER_COLOUR	 = 1u << 13,
};

enum cow_menu_item_type {
	COW_MENU_ITEM_NORMAL,	 /* selectable item with an action */
	COW_MENU_ITEM_SEPARATOR, /* horizontal rule, no action */
	COW_MENU_ITEM_TITLE,	 /* non-selectable label (bold heading) */
};

struct cow_menu_item {
	enum cow_menu_item_type type;
	char		       *label;	/* display text; NULL for separator */
	char		       *action; /* command string to run, or name of
					 * sub-menu when is_popup is true */
	bool	       is_popup;		 /* action is a sub-menu name */
	bool	       popup_pick_context; /* pick target before child actions */
	struct wl_list link;		/* in cow_menu.items */
};

struct cow_menu {
	char		      *name;  /* unique identifier used in config */
	struct cow_menu_style style; /* values for explicitly overridden fields */
	uint32_t	       style_overrides;
	struct wl_list       items; /* list of cow_menu_item */
	struct wl_list       link;  /* in cow_wm.menus */
};

struct cow_menu_popup {
	struct cow_menu *def;	   /* definition being rendered */
	int		 x, y;	   /* top-left screen position */
	int		 width;	   /* rendered width in pixels */
	int		 height;   /* rendered height in pixels */
	int		 item_h;   /* height of one item row */
	int		 selected; /* highlighted item index (-1 = none) */
	struct cow_menu_style style; /* resolved style snapshot */

	/* Created from the render hook. */
	struct wl_surface	      *surface;
	struct river_shell_surface_v1 *shell_surface;
	struct river_node_v1	      *node;
	struct cow_shm_buffer	      *buffer;
	bool			       committed; /* first commit done? */
	bool needs_create;			  /* defer create to render */
	bool needs_redraw;			  /* defer redraw to render */
	bool needs_layout;			  /* style changed geometry */
	bool needs_destroy;			  /* defer destroy to render */

	/* Popup nesting */
	struct cow_menu_popup *parent; /* NULL for the root popup */
	struct cow_menu_popup *child;  /* currently open sub-menu, or NULL */
	int child_item;		     /* item index which opened child */
	bool pick_target_on_action;    /* pick target before child actions */

	struct wl_list link; /* in cow_wm.open_menus or cow_wm.menu_graveyard */
};

/*
 * Find a menu definition by name. Returns NULL if not found.
 */
struct cow_menu *cow_menu_find(const char *name);

/*
 * Create a new named menu. Fails (returns NULL) if a menu with the
 * same name already exists.
 */
struct cow_menu *cow_menu_create(const char *name);

/*
 * Remove all items from a menu without destroying the menu itself.
 */
void cow_menu_clear(struct cow_menu *menu);

/*
 * Destroy a menu definition entirely, closing any open popup first.
 */
void cow_menu_destroy(struct cow_menu *menu);

/*
 * Destroy all menu definitions.
 */
void cow_menu_destroy_all(void);

/*
 * Add an item to a menu.
 */
struct cow_menu_item *cow_menu_item_add(struct cow_menu *menu,
    enum cow_menu_item_type type, const char *label, const char *action,
    bool is_popup, bool popup_pick_context);

/* Open a popup at the given screen position. */
struct cow_menu_popup *cow_menu_popup_open(struct cow_menu *menu, int x, int y,
    struct cow_view *context_view, struct cow_menu_popup *parent,
    bool centre_title_on_position);

/*
 * Close a single popup level (and recursively its child).
 */
void cow_menu_popup_close(struct cow_menu_popup *popup);

/*
 * Close all open popups.
 */
void cow_menu_close_all(void);

/*
 * Returns true if any menu popup is currently open.
 */
bool cow_menu_is_open(void);

/*
 * Called during the manage cycle to process pointer/keyboard input
 * while menus are open.  Returns true if input was consumed (i.e.
 * callers should skip normal window interaction dispatch).
 */
bool cow_menu_process_input(void);

/*
 * Called during the render cycle to position and commit all open
 * popup surfaces.
 */
void cow_menu_render_all(void);

/*
 * Keyboard navigation — called by the menu-key command.
 * direction is one of: "up", "down", "left", "right", "select", "close".
 */
void cow_menu_navigate(const char *direction);

/*
 * Re-render a single popup (e.g. after selection changes).
 */
void cow_menu_popup_redraw(struct cow_menu_popup *popup);

/* Populate a cow_menu_style with the built-in menu defaults. */
void cow_menu_style_default(struct cow_menu_style *);

/* Resolve global style plus the named menu's explicit overrides. */
void cow_menu_style_resolve(const struct cow_menu *, struct cow_menu_style *);

/* Refresh open popups after a global or per-menu style change. */
void cow_menu_style_refresh(struct cow_menu *, bool);

#endif /* COW_MENU_H */
