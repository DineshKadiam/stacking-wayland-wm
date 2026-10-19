#ifndef COW_CHORD_HINT_H
#define COW_CHORD_HINT_H

struct cow_seat;

void cow_chord_hint_show(struct cow_seat *, const char *);
void cow_chord_hint_hide(struct cow_seat *);
void cow_chord_hint_update(void);

#endif
