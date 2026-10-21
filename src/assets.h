// Index of every wallpaper, icon set, music track and lock screen across the
// installed themes, plus the pictures on the memory card, for the asset library.
#pragma once

#include "library.h"

typedef enum { ASSET_WALLPAPERS, ASSET_ICONS, ASSET_MUSIC, ASSET_LOCKSCREENS, ASSET_PICTURES, ASSET_KIND_COUNT } AssetKind;

#define ASSET_PREVIEW_ICONS 8

typedef struct {
	char dir[256];
	char title[128];
	char provider[64];
	char thumb[384];
	char icons[ASSET_PREVIEW_ICONS][512]; // first icons, for previews
	int icon_count;                       // all custom icons in the theme
} AssetTheme;

typedef struct {
	AssetKind kind;
	int theme; // index into AssetIndex.themes, or -1 for a picture
	int page;  // wallpapers: page index within its theme; pictures: scan order
	char folder[64]; // pictures: the folder it's in
	char image[512]; // what a card shows
	char file[512];  // full-size wallpaper / lock screen, or the .at9
} Asset;

typedef struct {
	AssetTheme *themes;
	int theme_count;
	Asset *items;
	int count;
	int per_kind[ASSET_KIND_COUNT];
} AssetIndex;

// Blocking; run on a worker thread. Reads theme.xml of each theme with files
// and looks for pictures.
void assets_scan(const LibraryItem *themes, int count, AssetIndex *out);
void assets_free(AssetIndex *index);
