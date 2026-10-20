#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cow/icon_lookup.h"
#include "cow/memory.h"

#define CHECK(test) do { \
	if (!(test)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
		exit(1); \
	} \
} while (0)

static bool
reject_png(const char *path)
{
	return strstr(path, ".png") == NULL;
}

static void
check(const char *name, int size, bool (*accept)(const char *),
    const char *root, const char *suffix)
{
	char *path = cow_icon_name_resolve(name, size, accept), *expected = NULL;

	if (suffix)
		xasprintf(&expected, "%s/%s", root, suffix);
	CHECK((!path && !expected) || (path && expected && !strcmp(path, expected)));
	free(path);
	free(expected);
}

int
main(void)
{
	char root[] = "/tmp/cow-icon-lookup.XXXXXX";
	const char *dirs[] = {
		"user", "user/icons", "user/icons/hicolor",
		"user/icons/hicolor/16x16", "user/icons/hicolor/64x64",
		"user/icons/hicolor/scalable", "user/pixmaps",
		"system", "system/pixmaps",
	};
	const char *files[] = {
		"user/icons/hicolor/16x16/example.png",
		"user/icons/hicolor/64x64/example.png",
		"user/icons/hicolor/scalable/example.svg",
		"user/pixmaps/fallback.xpm",
		"system/pixmaps/fallback.png",
		"system/pixmaps/system-only.png",
	};
	char *path;

	CHECK(mkdtemp(root));
	CHECK(chdir(root) == 0);
	for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
		CHECK(mkdir(dirs[i], 0700) == 0);
	for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
		FILE *fp = fopen(files[i], "w");

		CHECK(fp);
		CHECK(fclose(fp) == 0);
	}
	xasprintf(&path, "%s/user", root);
	CHECK(setenv("XDG_DATA_HOME", path, 1) == 0);
	free(path);
	xasprintf(&path, "%s/system", root);
	CHECK(setenv("XDG_DATA_DIRS", path, 1) == 0);
	free(path);
	check("example", 16, NULL, root, files[0]);
	check("example.png", 64, NULL, root, files[1]);
	check("example", 32, NULL, root, files[2]);
	check("example", 16, reject_png, root, files[2]);
	check("fallback", 16, NULL, root, files[3]);
	check("system-only", 16, NULL, root, files[5]);
	check("missing", 16, NULL, root, NULL);
	check("../example", 16, NULL, root, NULL);
	xasprintf(&path, "%s/%s", root, files[0]);
	check(path, 16, NULL, root, files[0]);
	check(path, 16, reject_png, root, NULL);
	free(path);
	xasprintf(&path, "%s/fifo.png", root);
	CHECK(mkfifo(path, 0600) == 0);
	check(path, 16, NULL, root, NULL);
	CHECK(unlink(path) == 0);
	free(path);
	for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++)
		CHECK(unlink(files[i]) == 0);
	for (size_t i = sizeof(dirs) / sizeof(*dirs); i > 0; i--)
		CHECK(rmdir(dirs[i - 1]) == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(root) == 0);
	return 0;
}
