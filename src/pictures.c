#include "pictures.h"

#include "fs.h"
#include "log.h"
#include "photos.h"

#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEPTH 4

typedef struct {
	PictureList *list;
	int capacity;
	char dir[384];
	char folder[64]; // label for files directly in dir
	int depth;
} Walk;

static int is_picture(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".png") == 0);
}

static void walk(Walk *w);

static int visit(const char *name, int is_dir, void *user) {
	Walk *w = user;
	if (w->list->count >= PICTURES_MAX)
		return 1;
	if (name[0] == '.')
		return 0;
	if (is_dir) {
		if (w->depth + 1 >= MAX_DEPTH)
			return 0;
		Walk sub = *w;
		snprintf(sub.dir, sizeof(sub.dir), "%s/%s", w->dir, name);
		snprintf(sub.folder, sizeof(sub.folder), "%s", name);
		sub.depth = w->depth + 1;
		walk(&sub);
		w->capacity = sub.capacity;
		return 0;
	}
	if (!is_picture(name) || strlen(w->dir) + strlen(name) + 2 > 250)
		return 0;
	char path[384];
	snprintf(path, sizeof(path), "%s/%s", w->dir, name);
	if (photos_is_shared_copy(path))
		return 0; // a theme wallpaper Imago put in Photos: it's already on the shelves
	PictureList *list = w->list;
	if (list->count == w->capacity) {
		int cap = w->capacity ? w->capacity * 2 : 64;
		Picture *grown = realloc(list->items, (size_t)cap * sizeof(Picture));
		if (!grown)
			return 1;
		list->items = grown;
		w->capacity = cap;
	}
	Picture *p = &list->items[list->count++];
	snprintf(p->path, sizeof(p->path), "%s/%s", w->dir, name);
	snprintf(p->folder, sizeof(p->folder), "%s", w->folder);
	return 0;
}

static void walk(Walk *w) {
	if (fs_list_dir(w->dir, visit, w) == 0 || w->depth > 0)
		return;
	// ux0:picture can be briefly unreadable while the system updates its
	// photo index (right after screenshots were deleted, say): try once more.
	sceKernelDelayThread(500 * 1000);
	if (fs_list_dir(w->dir, visit, w) != 0)
		log_write("pictures: couldn't list %s", w->dir);
}

static int by_folder_newest(const void *a, const void *b) {
	const Picture *x = a, *y = b;
	int c = strcasecmp(x->folder, y->folder);
	return c ? c : strcmp(y->path, x->path); // camera and screenshot names sort by date
}

void pictures_scan(PictureList *out) {
	memset(out, 0, sizeof(*out));
	fs_mkdir_p(PICTURES_DIR); // so there's an obvious place to copy pictures to
	static const struct {
		const char *dir, *label;
	} roots[] = {{"ux0:picture", "Photos"}, {PICTURES_DIR, "Imago"}};
	Walk w = {out, 0, "", "", 0};
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		snprintf(w.dir, sizeof(w.dir), "%s", roots[i].dir);
		snprintf(w.folder, sizeof(w.folder), "%s", roots[i].label);
		w.depth = 0;
		walk(&w);
	}
	if (out->count > 1)
		qsort(out->items, (size_t)out->count, sizeof(Picture), by_folder_newest);
}

void pictures_free(PictureList *list) {
	free(list->items);
	memset(list, 0, sizeof(*list));
}
