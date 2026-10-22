// Turning a source picture into a part's finished file.
#pragma once

#include "store.h"

#define EDIT_ERROR_SIZE STORE_ERROR_SIZE
// Source pictures are read at up to this size (big JPEGs at a reduced scale).
#define EDIT_MAX_SOURCE_PIXELS (2048L * 2048L)

// A rectangle of the source picture, in fractions of its width and height.
typedef struct {
	double x, y, w, h;
} Crop;

// The size a part's file must have: the app's current file's size when it
// looks sane, else the shell's standard size.
void edit_target_size(const char *id, ArtPart part, int *w, int *h);

// Renders `crop` of `source` (PNG or JPEG) at the part's size, reduces it to
// 256 colours and saves it as the part's custom file. For ART_BG with
// `seamless`, the start gate is cut from the same pixels at (gate_x, gate_y)
// of the background and saved too, so it matches the background exactly.
int edit_make(const char *id, ArtPart part, const char *source, Crop crop, int seamless, int gate_x, int gate_y,
              char *err);
