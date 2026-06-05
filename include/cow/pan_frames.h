/* pan_frames.h — public API for edge-scroll pan frame surfaces */
#ifndef COW_PAN_FRAMES_H
#define COW_PAN_FRAMES_H

#include <stdbool.h>

struct cow_output;

/*
 * Edge directions for the pan-frame machinery.  The integer values
 * are stable: pan_edge fields (output->pan_edge), the per-edge arrays
 * inside cow_output, and several callers index by these values.
 */
enum cow_pan_edge {
	COW_PAN_TOP    = 0,
	COW_PAN_BOTTOM = 1,
	COW_PAN_LEFT   = 2,
	COW_PAN_RIGHT  = 3,
};

#define COW_PAN_EDGE_BIT(e) (1u << (e))

/*
 * Bitmask of edges along which a page is available from (cur_col,
 * cur_row) in a (cols x rows) page grid.  Each bit is COW_PAN_EDGE_BIT
 * of an enum cow_pan_edge value; callers test with
 * (mask & COW_PAN_EDGE_BIT(COW_PAN_LEFT)) etc.
 */
unsigned cow_pan_edge_mask(int, int, int, int);

void pan_frames_init(void);
void pan_frames_update_output(struct cow_output *);
void pan_frames_update_all(void);
void pan_frames_destroy_output(struct cow_output *);
void pan_frames_destroy_all(void);

/* Paint a specific edge orange (true) or transparent (false). */
void pan_frame_set_orange(struct cow_output *, int, bool);

/*
 * Called from the manage cycle when edge_scroll is enabled.  Fires a
 * page switch if the pointer has dwelt at an edge long enough.
 * pan_edge is set either by the pan-frame wl_pointer listener
 * (primary) or by cow_output_pan_pointer_leave (fallback when no
 * wl_layer_shell).
 */
void cow_output_pan_frame_tick(struct cow_output *);

/*
 * Fallback proximity-based edge detection -- only active when
 * wl_layer_shell is unavailable (pan frames cannot be created).
 * Called on River pointer_leave / pointer_enter events from seat.c.
 */
void cow_output_pan_pointer_leave(int, int);
void cow_output_pan_pointer_enter(void);

/*
 * Op-mode proximity-based edge arming.  Layer-surface pointer events
 * stop firing once river enters pointer-op mode for a drag/resize, so
 * pan frames cannot detect entry the usual way.  Called from the
 * pointer-op processing loop with the current seat pointer position;
 * arms pan_edge by cursor position only.
 */
void cow_output_pan_op_arm(int, int);

#endif /* COW_PAN_FRAMES_H */
