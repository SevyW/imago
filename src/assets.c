#include "assets.h"

#include "fs.h"
#include "pictures.h"
#include "themedoc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Asset *add(AssetIndex *idx, int *capacity, AssetKind kind, int theme) {
	if (idx->count == *capacity) {
		int cap = *capacity ? *capacity * 2 : 64;
		Asset *grown = realloc(idx->items, (size_t)cap * sizeof(Asset));
		if (!grown)
			return NULL;
		idx->items = grown;
		*capacity = cap;
	}
	Asset *a = &idx->items[idx->count++];
	memset(a, 0, sizeof(*a));
	a->kind = kind;
	a->theme = theme;
	idx->per_kind[kind]++;
	return a;
}

static int by_kind(const void *a, const void *b) {
	const Asset *x = a, *y = b;
	if (x->kind != y->kind)
		return (int)x->kind - (int)y->kind;
	if (x->theme != y->theme)
		return x->theme - y->theme;
	return x->page - y->page;
}

void assets_scan(const LibraryItem *themes, int count, AssetIndex *idx) {
	memset(idx, 0, sizeof(*idx));
	idx->themes = calloc(count > 0 ? (size_t)count : 1, sizeof(AssetTheme));
	if (!idx->themes)
		return;
	int capacity = 0;
	for (int i = 0; i < count; i++) {
		if (!themes[i].has_files)
			continue;
		ThemeDoc doc;
		if (themedoc_load(&doc, themes[i].dir) != 0)
			continue;
		int t = idx->theme_count++;
		AssetTheme *at = &idx->themes[t];
		snprintf(at->dir, sizeof(at->dir), "%s", themes[i].dir);
		snprintf(at->title, sizeof(at->title), "%s", themes[i].title);
		snprintf(at->provider, sizeof(at->provider), "%s", themes[i].provider);
		snprintf(at->thumb, sizeof(at->thumb), "%s", themes[i].thumb);

		int pages = themedoc_page_count(&doc);
		for (int p = 0; p < pages; p++) {
			char image[512], thumb[512];
			if (themedoc_page_file(&doc, p, 0, image, sizeof(image)) != 0 || !fs_exists(image))
				continue;
			themedoc_page_file(&doc, p, 1, thumb, sizeof(thumb));
			Asset *a = add(idx, &capacity, ASSET_WALLPAPERS, t);
			if (!a)
				break;
			a->page = p;
			snprintf(a->file, sizeof(a->file), "%s", image);
			snprintf(a->image, sizeof(a->image), "%s", fs_exists(thumb) ? thumb : image);
		}

		char icons[24][512];
		int n = themedoc_icon_files(&doc, icons, 24), shown = 0;
		for (int k = 0; k < n && shown < ASSET_PREVIEW_ICONS; k++)
			if (fs_exists(icons[k]))
				snprintf(at->icons[shown++], sizeof(at->icons[0]), "%s", icons[k]);
		at->icon_count = n;
		if (shown > 0) {
			Asset *a = add(idx, &capacity, ASSET_ICONS, t);
			if (a)
				snprintf(a->image, sizeof(a->image), "%s", at->icons[0]);
		}

		char file[512];
		themedoc_music_file(&doc, file, sizeof(file));
		if (file[0] && fs_exists(file)) {
			Asset *a = add(idx, &capacity, ASSET_MUSIC, t);
			if (a) {
				snprintf(a->file, sizeof(a->file), "%s", file);
				snprintf(a->image, sizeof(a->image), "%s", at->thumb);
			}
		}

		themedoc_lockscreen_file(&doc, file, sizeof(file));
		if (file[0] && fs_exists(file)) {
			Asset *a = add(idx, &capacity, ASSET_LOCKSCREENS, t);
			if (a) {
				snprintf(a->file, sizeof(a->file), "%s", file);
				snprintf(a->image, sizeof(a->image), "%s", file);
			}
		}
		themedoc_free(&doc);
	}

	PictureList pictures;
	pictures_scan(&pictures);
	for (int i = 0; i < pictures.count; i++) {
		Asset *a = add(idx, &capacity, ASSET_PICTURES, -1);
		if (!a)
			break;
		a->page = i;
		snprintf(a->file, sizeof(a->file), "%s", pictures.items[i].path);
		snprintf(a->image, sizeof(a->image), "%s", pictures.items[i].path);
		snprintf(a->folder, sizeof(a->folder), "%s", pictures.items[i].folder);
	}
	pictures_free(&pictures);

	if (idx->count > 1)
		qsort(idx->items, (size_t)idx->count, sizeof(Asset), by_kind);
}

void assets_free(AssetIndex *idx) {
	free(idx->themes);
	free(idx->items);
	memset(idx, 0, sizeof(*idx));
}
