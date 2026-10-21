// Access to the system theme table (tbl_theme in ur0:shell/db/app.db).
// Requires "unsafe homebrew" to be enabled in HENkaku settings.
//
// Functions that can fail take an `err` buffer of THEMEDB_ERROR_SIZE bytes,
// so concurrent callers never see each other's messages.
#pragma once

#include <stdint.h>

#include "theme.h"

#ifndef APP_DB_PATH // overridden by host-side tests
#define APP_DB_PATH "ur0:shell/db/app.db"
#endif

#define THEMEDB_ERROR_SIZE 160

typedef struct {
	char id[256]; // theme folder exactly as stored, e.g. "ux0:customtheme/Foo"
	char title[128];
	char provider[64];
	char package_image[128];
	int64_t size;
} DbTheme;

// 1 when app.db can be opened for writing.
int themedb_writable(void);
// Lists every row of tbl_theme. *themes must be freed by the caller.
int themedb_list(DbTheme **themes, int *count, char *err);
// Adds (or replaces) the row for a theme folder.
int themedb_register(const char *dir, const ThemeXml *xml, int64_t size, char *err);
// Removes the row(s) for a theme folder. Returns the number removed, or -1.
int themedb_unregister(const char *dir, char *err);

// Runs SQLite's quick_check on a database file. 0 when it is healthy.
int themedb_check(const char *path, char *err);
// Copies a healthy app.db to dest_path (written to a temp file first).
int themedb_backup(const char *dest_path, char *err);
// Replaces app.db with a healthy backup. The current file is kept as
// app.db.imago-old until the swap succeeds; nothing is truncated in place.
int themedb_restore(const char *src_path, char *err);
