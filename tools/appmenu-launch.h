#ifndef COW_APPMENU_LAUNCH_H
#define COW_APPMENU_LAUNCH_H

#include "cJSON.h"

/* Owned argument array, or NULL for an unsupported/invalid Exec line. */
cJSON *appmenu_arguments(const cJSON *app);
int appmenu_launch(const char *file, const char *terminal);

#endif
