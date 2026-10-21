// An editable theme: theme.xml as a tree whose nodes remember which theme
// folder they came from, so parts of several themes can be combined and then
// written out as a brand-new theme folder.
#pragma once

#include <stddef.h>

#include "xmltree.h"

#define THEMEDOC_MAX_ORIGINS 16
#define THEMEDOC_MAX_PAGES 10

typedef enum {
	PART_WALLPAPERS,
	PART_MUSIC,
	PART_ICONS,
	PART_LOCKSCREEN,
	PART_STATUSBAR,
	PART_COUNT
} ThemePart;

typedef struct {
	XmlNode *root;
	int origin_count;
	char origins[THEMEDOC_MAX_ORIGINS][256];   // source theme folders
	char origin_titles[THEMEDOC_MAX_ORIGINS][128];
	char origin_providers[THEMEDOC_MAX_ORIGINS][64];
	// Folders of pictures from the memory card rather than themes: their
	// files are converted to theme images when the theme is written.
	unsigned char origin_pictures[THEMEDOC_MAX_ORIGINS];
} ThemeDoc;

// Theme image sizes, as published themes use them.
#define THEME_WALLPAPER_W 960
#define THEME_WALLPAPER_H 512
#define THEME_PAGE_THUMB_W 360
#define THEME_PAGE_THUMB_H 192

const char *themedoc_part_name(ThemePart part);

int themedoc_load(ThemeDoc *doc, const char *dir);
// A complete minimal theme made from pictures (JPEG or PNG paths): one
// wallpaper page per picture, and lock_picture (or the first page) as the
// lock screen. Either may be empty, not both.
int themedoc_from_pictures(ThemeDoc *doc, const char *const *pictures, int count, const char *lock_picture);
void themedoc_free(ThemeDoc *doc);
// A deep copy (for editing without touching the original).
int themedoc_copy(ThemeDoc *dst, const ThemeDoc *src);

void themedoc_get_title(const ThemeDoc *doc, char *out, size_t size);
void themedoc_set_title(ThemeDoc *doc, const char *title);
void themedoc_set_provider(ThemeDoc *doc, const char *provider);

int themedoc_has_part(const ThemeDoc *doc, ThemePart part);
// Origin index providing a part, or -1 when the theme doesn't have it.
int themedoc_part_origin(const ThemeDoc *doc, ThemePart part);
// Music and icons can be switched off (icons fall back to the system ones).
int themedoc_remove_part(ThemeDoc *doc, ThemePart part);
// Replaces a part with a copy of the same part from another theme.
int themedoc_take_part(ThemeDoc *doc, ThemePart part, const ThemeDoc *from);
// Replaces only the lock screen picture, keeping this theme's clock and
// notification colours.
int themedoc_take_lockscreen_image(ThemeDoc *doc, const ThemeDoc *from);
// Credits every theme whose parts are used: "akane, MajorTom87".
void themedoc_credit(const ThemeDoc *doc, char *out, size_t size);

int themedoc_page_count(const ThemeDoc *doc);
// Absolute path of a page's wallpaper or its thumbnail.
int themedoc_page_file(const ThemeDoc *doc, int page, int thumbnail, char *out, size_t size);
// Keeps only the pages whose keep[i] is nonzero (at least one must remain).
int themedoc_keep_pages(ThemeDoc *doc, const int *keep, int count);
int themedoc_icon_count(const ThemeDoc *doc);
// Absolute paths of the custom icon files (up to max). Returns the count.
int themedoc_icon_files(const ThemeDoc *doc, char (*out)[512], int max);
// Replaces the wallpapers with the given pages, each taken from its own
// theme (froms[i] page pages[i]); up to THEMEDOC_MAX_PAGES.
int themedoc_set_pages(ThemeDoc *doc, const ThemeDoc *const *froms, const int *pages, int count);
// Absolute path of a file referenced by a *FilePath node.
int themedoc_node_file(const ThemeDoc *doc, const XmlNode *node, char *out, size_t size);
// Absolute paths of the music file and the lock screen image ("" if none).
void themedoc_music_file(const ThemeDoc *doc, char *out, size_t size);
void themedoc_lockscreen_file(const ThemeDoc *doc, char *out, size_t size);

// Called after each copied file; return nonzero to cancel.
typedef int (*ThemeDocProgress)(void *user, int done, int total);

// Writes the theme into dest_dir (which must not exist yet): copies every
// referenced file, renaming any that collide, and writes theme.xml. With
// new_previews, the home/lock/thumbnail previews are generated from the
// first wallpaper and the lock screen image.
int themedoc_write(const ThemeDoc *doc, const char *dest_dir, int new_previews, ThemeDocProgress progress,
                   void *user, char *err, size_t err_size);
