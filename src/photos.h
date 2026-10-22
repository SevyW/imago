// Shares the wallpapers and lock screens of installed themes with the Photos
// app (optional, Settings), so the system's own per-page wallpaper picker can
// use them: for example on pages 11 and up with livearea_nolimits, which no
// theme can reach. Pictures go in through the system's PhotoExport library;
// Imago remembers each one it added, never adds one twice, and only ever
// deletes its own.
#pragma once

#include "config.h"
#include "library.h"
#include "themedb.h"

#include <stddef.h>

#define PHOTOS_MANIFEST DATA_DIR "/photos.txt"

// Blocking; run on a worker thread. Adds what isn't shared yet. Returns how
// many pictures were added, or -1 with err set when nothing could be.
int photos_share(const LibraryItem *themes, int count, char *err, size_t err_size);
// Blocking. Deletes the pictures Imago added to Photos; returns how many.
int photos_unshare(void);
// How many pictures Imago has added (reads the manifest).
int photos_shared_count(void);

// One theme's pictures (theme_dir: its folder, e.g. ux0:customtheme/Foo).
int photos_theme_count(const char *theme_dir);
// Blocking. Deletes just that theme's pictures; returns how many.
int photos_unshare_theme(const char *theme_dir);
// Which home-screen pages show one of its pictures from Photos as their
// wallpaper: their numbers go to out. Returns how many.
int photos_theme_pages(const char *theme_dir, const DbPage *pages, int page_count, int *out, int max);
// Whether path is one of the copies Imago put in Photos.
int photos_is_shared_copy(const char *path);
