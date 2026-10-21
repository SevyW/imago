#include "library.h"

#include "config.h"
#include "fs.h"
#include "theme.h"
#include "themedb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int push(Library *lib, int *capacity, const LibraryItem *item) {
	if (lib->count == *capacity) {
		int cap = *capacity ? *capacity * 2 : 32;
		LibraryItem *grown = realloc(lib->items, (size_t)cap * sizeof(LibraryItem));
		if (!grown)
			return -1;
		lib->items = grown;
		*capacity = cap;
	}
	lib->items[lib->count++] = *item;
	return 0;
}

static void normalize(char *dir) {
	size_t n = strlen(dir);
	while (n > 0 && dir[n - 1] == '/')
		dir[--n] = '\0';
}

// Official themes from the PlayStation Store live in *:theme/ and are managed
// by the system, so they are not listed.
static int is_official(const char *dir) {
	return strstr(dir, ":theme/") != NULL || strncmp(dir, "pd0:", 4) == 0 ||
	       strncmp(dir, "vs0:", 4) == 0;
}

static void fill_from_folder(LibraryItem *item) {
	ThemeXml xml;
	char thumb_rel[128] = "";
	if (theme_xml_load(item->dir, &xml) == 0) {
		item->has_files = 1;
		if (!item->title[0])
			snprintf(item->title, sizeof(item->title), "%s", xml.title);
		if (!item->provider[0])
			snprintf(item->provider, sizeof(item->provider), "%s", xml.provider);
		snprintf(thumb_rel, sizeof(thumb_rel), "%s", xml.package_image);
	}
	if (!item->thumb[0] && item->has_files)
		snprintf(item->thumb, sizeof(item->thumb), "%s/%s", item->dir,
		         thumb_rel[0] ? thumb_rel : "preview_thumbnail.png");
	ThemeMarker marker;
	if (theme_marker_read(item->dir, &marker) == 0) {
		item->psvt_id = marker.id;
		snprintf(item->psvt_hash, sizeof(item->psvt_hash), "%s", marker.hash);
	}
	item->deletable = strstr(item->dir, ":customtheme/") != NULL;
	if (!item->title[0]) {
		const char *slash = strrchr(item->dir, '/');
		snprintf(item->title, sizeof(item->title), "%s", slash ? slash + 1 : item->dir);
	}
}

typedef struct {
	Library *lib;
	int *capacity;
	const char *base;
} FolderScan;

static int on_folder(const char *name, int is_dir, void *user) {
	FolderScan *scan = user;
	if (!is_dir || strstr(name, ".imago-new") || strstr(name, ".imago-old"))
		return 0;
	LibraryItem item;
	memset(&item, 0, sizeof(item));
	snprintf(item.dir, sizeof(item.dir), "%s/%s", scan->base, name);
	for (int i = 0; i < scan->lib->count; i++)
		if (strcasecmp(scan->lib->items[i].dir, item.dir) == 0)
			return 0; // already listed from the database
	fill_from_folder(&item);
	if (item.has_files)
		push(scan->lib, scan->capacity, &item);
	return 0;
}

static int by_title(const void *a, const void *b) {
	return strcasecmp(((const LibraryItem *)a)->title, ((const LibraryItem *)b)->title);
}

void library_scan(Library *lib) {
	memset(lib, 0, sizeof(*lib));
	int capacity = 0;

	DbTheme *rows = NULL;
	int count = 0;
	char err[THEMEDB_ERROR_SIZE] = "";
	lib->db_ok = themedb_list(&rows, &count, err) == 0;
	if (!lib->db_ok)
		snprintf(lib->db_error, sizeof(lib->db_error), "%s", err);
	for (int i = 0; i < count; i++) {
		LibraryItem item;
		memset(&item, 0, sizeof(item));
		snprintf(item.dir, sizeof(item.dir), "%s", rows[i].id);
		normalize(item.dir);
		if (is_official(item.dir))
			continue;
		snprintf(item.title, sizeof(item.title), "%s", rows[i].title);
		snprintf(item.provider, sizeof(item.provider), "%s", rows[i].provider);
		if (rows[i].package_image[0])
			snprintf(item.thumb, sizeof(item.thumb), "%s/%s", item.dir, rows[i].package_image);
		item.size = rows[i].size;
		item.registered = 1;
		fill_from_folder(&item);
		push(lib, &capacity, &item);
	}
	free(rows);

	for (int p = 0; p < PARTITION_COUNT; p++) {
		char base[32];
		snprintf(base, sizeof(base), "%scustomtheme", config_partition_device(p));
		FolderScan scan = {lib, &capacity, base};
		fs_list_dir(base, on_folder, &scan);
	}

	if (lib->count > 1)
		qsort(lib->items, (size_t)lib->count, sizeof(LibraryItem), by_title);
}

void library_free(Library *lib) {
	free(lib->items);
	memset(lib, 0, sizeof(*lib));
}

ThemeStatus library_status(const Library *lib, uint32_t id, const char *hash, const char *title) {
	const LibraryItem *by_name = NULL;
	for (int i = 0; i < lib->count; i++) {
		const LibraryItem *it = &lib->items[i];
		if (!it->registered)
			continue;
		if (it->psvt_id == id)
			return it->psvt_hash[0] && strcmp(it->psvt_hash, hash) != 0 ? THEME_UPDATE : THEME_INSTALLED;
		if (!by_name && !it->psvt_id && strcasecmp(it->title, title) == 0)
			by_name = it; // installed by another manager; no hash to compare
	}
	return by_name ? THEME_INSTALLED : THEME_NOT_INSTALLED;
}
