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
// Saves indices (one per pixel) with an RGBA palette of `colors` entries.
int image_save_png_palette(const char *path, const uint8_t *indices, int width, int height, const uint8_t *palette,
                           int colors);
// Reads a PNG's dimensions from its header without decoding it.
int image_png_size(const char *path, int *width, int *height);
// Loads a PNG or JPEG of at most max_pixels: big JPEGs are decoded at a
// reduced scale (jpegio.c), bigger PNGs are refused.
int image_load_limited(const char *path, uint8_t **pixels, int *width, int *height, long max_pixels);
// Scales the source rectangle (rx, ry, rw, rh, in source pixels) to dw x dh:
// area averaging when shrinking, bilinear when enlarging. Returns a malloc'd
// RGBA buffer or NULL.
uint8_t *image_resize_region(const uint8_t *src, int sw, int sh, double rx, double ry, double rw, double rh, int dw,
                             int dh);
// Scales src to fill dw x dh, cropping the overflow evenly ("cover"), using
// area averaging. Returns a malloc'd RGBA buffer or NULL.
uint8_t *image_resize_cover(const uint8_t *src, int sw, int sh, int dw, int dh);
