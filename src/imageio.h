// Decoding, resizing and encoding images in memory (theme previews, and
// pictures from the memory card turned into wallpapers).
#pragma once

#include <stdint.h>

// Loads a PNG as 8-bit RGBA. *pixels must be freed by the caller.
int image_load_png(const char *path, uint8_t **pixels, int *width, int *height);
int image_save_png(const char *path, const uint8_t *rgba, int width, int height);
// Loads a JPEG as 8-bit RGBA (jpegio.c). Large photos are decoded at a
// reduced scale that still covers min_width x min_height.
int image_load_jpeg(const char *path, uint8_t **pixels, int *width, int *height, int min_width, int min_height);
// Loads a PNG or a JPEG, whichever the file is.
int image_load_any(const char *path, uint8_t **pixels, int *width, int *height, int min_width, int min_height);
// Saves an opaque 8-bit palette PNG (up to 256 colours, dithered), the format
// published themes use for wallpapers and lock screens.
int image_save_png_indexed(const char *path, const uint8_t *rgba, int width, int height);
// Scales src to fill dw x dh, cropping the overflow evenly ("cover"), using
// area averaging. Returns a malloc'd RGBA buffer or NULL.
uint8_t *image_resize_cover(const uint8_t *src, int sw, int sh, int dw, int dh);
