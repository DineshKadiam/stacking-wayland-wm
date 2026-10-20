#ifndef COW_ICON_LOOKUP_H
#define COW_ICON_LOOKUP_H

#include <stdbool.h>

/*
 * Return an owned PNG/SVG/XPM path, or NULL. Search hicolor and pixmaps
 * in XDG data directories. An optional callback rejects unusable images.
 */
char *cow_icon_name_resolve(const char *, int, bool (*)(const char *));

#endif
