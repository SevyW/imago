// Shares the wallpapers and lock screens of installed themes with the Photos
// app (optional, Settings), so the system's own per-page wallpaper picker can
// use them: for example on pages 11 and up with livearea_nolimits, which no
// theme can reach. Pictures go in through the system's PhotoExport library;
// Imago remembers each one it added, never adds one twice, and only ever
// deletes its own.
#pragma once

#include "config.h"
#include "library.h"

#include <stddef.h>

#define PHOTOS_MANIFEST DATA_DIR "/photos.txt"

// Blocking; run on a worker thread. Adds what isn't shared yet. Returns how
// many pictures were added, or -1 with err set when nothing could be.
int photos_share(const LibraryItem *themes, int count, char *err, size_t err_size);
// Blocking. Deletes the pictures Imago added to Photos; returns how many.
int photos_unshare(void);
// How many pictures Imago has added (reads the manifest).
int photos_shared_count(void);
// Whether path is one of the copies Imago put in Photos.
int photos_is_shared_copy(const char *path);
