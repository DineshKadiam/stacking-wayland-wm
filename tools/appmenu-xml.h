#ifndef COW_APPMENU_XML_H
#define COW_APPMENU_XML_H

#include <libxml/tree.h>

/* Read, validate and merge local menu files. The caller owns the document. */
xmlDoc *appmenu_load(const char *path,
    int (*validate)(xmlNode *, unsigned));
int appmenu_parent_merge(const xmlNode *node);

#endif
