#ifndef COW_APPMENU_MENU_H
#define COW_APPMENU_MENU_H

#include "cJSON.h"

/* Returns an owned menu tree, or NULL with a diagnostic on stderr. */
cJSON *appmenu_resolve(const char *path, cJSON *applications);

#endif
