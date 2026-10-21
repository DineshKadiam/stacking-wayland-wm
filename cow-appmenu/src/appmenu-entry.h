#ifndef COW_APPMENU_ENTRY_H
#define COW_APPMENU_ENTRY_H

#include "cJSON.h"

void appmenu_locale_init(void);
void appmenu_locale_finish(void);
/* Owned arrays, with highest-priority directory first. */
cJSON *appmenu_data_dirs(const char *suffix);
cJSON *appmenu_config_dirs(const char *suffix);
/* Return 1 if found, 0 if absent, or -1 on error; path is owned when found. */
int appmenu_find_file(const cJSON *dirs, const char *name, int start, char **path);
int appmenu_find_menu(char **path);
cJSON *appmenu_catalogue(const cJSON *dirs);
cJSON *appmenu_application(const char *path);
int appmenu_program_available(const char *command);
char *appmenu_unescape(const char *text);
/* Owned metadata; NULL for an absent or invalid directory entry. */
cJSON *appmenu_directory(const char *path);

#endif
