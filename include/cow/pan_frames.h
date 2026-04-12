/* pan_frames.h — public API for edge-scroll pan frame surfaces */
#ifndef COW_PAN_FRAMES_H
#define COW_PAN_FRAMES_H

#include <stdbool.h>

struct cow_output;

void pan_frames_init(void);
void pan_frames_update_output(struct cow_output *output);
void pan_frames_update_all(void);
void pan_frames_destroy_output(struct cow_output *output);
void pan_frames_destroy_all(void);

/* Paint a specific edge orange (true) or transparent (false). */
void pan_frame_set_orange(struct cow_output *output, int edge, bool orange);

#endif /* COW_PAN_FRAMES_H */
