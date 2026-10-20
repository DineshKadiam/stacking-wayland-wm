#ifndef COW_APPMENU_LAYOUT_H
#define COW_APPMENU_LAYOUT_H

#include <libxml/tree.h>
#include "cJSON.h"

int appmenu_layout_validate(xmlNode *node);
void appmenu_layout(xmlNode *node, cJSON *menu, const cJSON *catalogue,
    xmlNode *inherited);

#endif
