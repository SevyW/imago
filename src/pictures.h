// Pictures on the memory card that can become wallpapers and lock screens:
// the Photos app's folders, and Imago's own folder for images copied from a PC.
#pragma once

#include "config.h"

#define PICTURES_DIR DATA_DIR "/pictures"
#define PICTURES_MAX 2000

typedef struct {
	char path[384];
	char folder[64]; // the folder it's in, for grouping ("SCREENSHOT", "Imago"...)
} Picture;

typedef struct {
	Picture *items;
	int count;
} PictureList;

// Blocking; run it on a worker thread. JPEG and PNG files, grouped by folder,
// newest names first within a folder.
void pictures_scan(PictureList *out);
void pictures_free(PictureList *list);
