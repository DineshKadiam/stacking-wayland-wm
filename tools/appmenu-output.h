#ifndef COW_APPMENU_OUTPUT_H
#define COW_APPMENU_OUTPUT_H

#include "cJSON.h"

int appmenu_output(const cJSON *menu, const cJSON *catalogue,
    const char *name, const char *terminal, int icon_size);

#endif
