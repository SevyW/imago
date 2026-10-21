// Decoding, resizing and encoding PNG images in memory (for theme previews).
#pragma once

#include <stdint.h>

// Loads a PNG as 8-bit RGBA. *pixels must be freed by the caller.
int image_load_png(const char *path, uint8_t **pixels, int *width, int *height);
int image_save_png(const char *path, const uint8_t *rgba, int width, int height);
// Scales src to fill dw x dh, cropping the overflow evenly ("cover"), using
// area averaging. Returns a malloc'd RGBA buffer or NULL.
uint8_t *image_resize_cover(const uint8_t *src, int sw, int sh, int dw, int dh);
