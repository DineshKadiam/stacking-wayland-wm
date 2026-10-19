#ifndef COW_NOTIFICATION_H
#define COW_NOTIFICATION_H

#include <stdbool.h>

struct cow_notification;
struct cow_view;
struct cow_output;

/* A non-NULL owner is cleared on dismissal; private notices have no name. */
void cow_notification_show(const char *, struct cow_notification **,
    const char *, struct cow_view *, struct cow_output *, int);
bool cow_notification_dismiss(const char *);
void cow_notification_hide(struct cow_notification *);
void cow_notification_clear(void);
int cow_notification_next_ms(void);
void cow_notification_render_all(void);

#endif
