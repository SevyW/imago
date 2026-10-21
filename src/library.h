// Installed custom themes: tbl_theme rows merged with *:customtheme folders.
#pragma once

#include <stdint.h>

typedef struct {
	char dir[256];
	char title[128];
	char provider[64];
	char thumb[384]; // full path to the theme's preview thumbnail
	long long size;
	int registered;  // has a row in tbl_theme
	int has_files;   // folder with a theme.xml exists
	int deletable;   // lives in a customtheme folder, so Imago may delete it
	uint32_t psvt_id; // from Imago's marker file, 0 if unknown
	char psvt_hash[41];
} LibraryItem;

typedef struct {
	LibraryItem *items;
	int count;
	int db_ok; // tbl_theme could be read
	char db_error[160];
} Library;

// Blocking; run it on a worker thread.
void library_scan(Library *out);
void library_free(Library *lib);

typedef enum { THEME_NOT_INSTALLED, THEME_INSTALLED, THEME_UPDATE } ThemeStatus;

// Matches a repository theme against a library (by marker id, then title).
ThemeStatus library_status(const Library *lib, uint32_t id, const char *hash, const char *title);
