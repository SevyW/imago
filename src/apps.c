#include "apps.h"

#include "appdb.h"
#include "fs.h"
#include "livearea.h"
#include "sfo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	AppList *list;
	int capacity;
} ScanCtx;

static int add_folder(const char *name, int is_dir, void *user) {
	ScanCtx *c = user;
	if (!is_dir || !store_valid_id(name))
		return 0;
	if (c->list->count == c->capacity) {
		int capacity = c->capacity ? c->capacity * 2 : 64;
		AppItem *grown = realloc(c->list->items, (size_t)capacity * sizeof(AppItem));
		if (!grown)
			return 1;
		c->list->items = grown;
		c->capacity = capacity;
	}
	AppItem *it = &c->list->items[c->list->count++];
	memset(it, 0, sizeof(*it));
	snprintf(it->id, sizeof(it->id), "%s", name);
	it->system = strncmp(name, "NPXS", 4) == 0;
	return 0;
}

// Falls back to the app's own param.sfo (readable for homebrew) when the
// database has no title for it.
static void title_from_sfo(AppItem *it) {
	static const char *const roots[] = {"ux0:app", "gro0:app", "ur0:app", "vs0:app"};
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]) && !it->title[0]; i++) {
		char path[256];
		snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", roots[i], it->id);
		if (sfo_read_string(path, "STITLE", it->title, sizeof(it->title)) != 0 || !it->title[0])
			sfo_read_string(path, "TITLE", it->title, sizeof(it->title));
	}
}

static int compare_items(const void *a, const void *b) {
	const AppItem *x = a, *y = b;
	if (x->system != y->system)
		return x->system - y->system;
	const char *s = x->title, *t = y->title;
	for (; *s && *t; s++, t++) {
		int d = tolower((unsigned char)*s) - tolower((unsigned char)*t);
		if (d)
			return d;
	}
	return (unsigned char)*s - (unsigned char)*t;
}

int apps_scan(AppList *out) {
	memset(out, 0, sizeof(*out));
	ScanCtx ctx = {out, 0};
	if (fs_list_dir(APPMETA_DIR, add_folder, &ctx) != 0)
		return -1;

	AppTitle *titles = NULL;
	int title_count = 0;
	appdb_titles(&titles, &title_count);
	for (int i = 0; i < out->count; i++) {
		AppItem *it = &out->items[i];
		for (int k = 0; k < title_count; k++) {
			if (strcmp(titles[k].id, it->id) == 0) {
				snprintf(it->title, sizeof(it->title), "%s", titles[k].title);
				break;
			}
		}
		if (!it->title[0])
			title_from_sfo(it);
		if (!it->title[0])
			snprintf(it->title, sizeof(it->title), "%s", it->id);

		LiveArea la;
		it->has_livearea = appdb_livearea(it->id, &la) == 0 && (la.bg_count > 0 || la.gate_count > 0);
		it->state = store_state(it->id);
	}
	free(titles);
	qsort(out->items, (size_t)out->count, sizeof(AppItem), compare_items);
	return 0;
}

void apps_free(AppList *list) {
	free(list->items);
	memset(list, 0, sizeof(*list));
}

const AppItem *apps_find(const AppList *list, const char *id) {
	for (int i = 0; i < list->count; i++)
		if (strcmp(list->items[i].id, id) == 0)
			return &list->items[i];
	return NULL;
}
