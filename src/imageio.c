#include "imageio.h"

#include "fs.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>

int image_load_png(const char *path, uint8_t **pixels, int *width, int *height) {
	char *data;
	size_t size;
	if (fs_read_file(path, &data, &size) != 0)
		return -1;
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	int rc = -1;
	if (png_image_begin_read_from_memory(&image, data, size)) {
		image.format = PNG_FORMAT_RGBA;
		if (image.width > 0 && image.height > 0 && image.width <= 4096 && image.height <= 4096) {
			uint8_t *buffer = malloc(PNG_IMAGE_SIZE(image));
			if (buffer && png_image_finish_read(&image, NULL, buffer, 0, NULL)) {
				*pixels = buffer;
				*width = (int)image.width;
				*height = (int)image.height;
				rc = 0;
			} else {
				free(buffer);
			}
		}
	}
	png_image_free(&image);
	free(data);
	return rc;
}

int image_save_png(const char *path, const uint8_t *rgba, int width, int height) {
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = (png_uint_32)width;
	image.height = (png_uint_32)height;
	image.format = PNG_FORMAT_RGBA;
	int ok = png_image_write_to_file(&image, path, 0, rgba, 0, NULL);
	png_image_free(&image);
	return ok ? 0 : -1;
}

uint8_t *image_resize_cover(const uint8_t *src, int sw, int sh, int dw, int dh) {
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return NULL;
	uint8_t *out = malloc((size_t)dw * (size_t)dh * 4);
	if (!out)
		return NULL;
	// Source rectangle with the destination's aspect ratio, centred.
	double scale = (double)dw / sw > (double)dh / sh ? (double)dw / sw : (double)dh / sh;
	double cw = dw / scale, ch = dh / scale;
	double ox = (sw - cw) / 2.0, oy = (sh - ch) / 2.0;
	double step_x = cw / dw, step_y = ch / dh;
	for (int y = 0; y < dh; y++) {
		int y0 = (int)(oy + y * step_y), y1 = (int)(oy + (y + 1) * step_y);
		if (y1 <= y0)
			y1 = y0 + 1;
		if (y1 > sh)
			y1 = sh;
		for (int x = 0; x < dw; x++) {
			int x0 = (int)(ox + x * step_x), x1 = (int)(ox + (x + 1) * step_x);
			if (x1 <= x0)
				x1 = x0 + 1;
			if (x1 > sw)
				x1 = sw;
			unsigned sum[4] = {0, 0, 0, 0}, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const uint8_t *row = src + ((size_t)yy * sw + x0) * 4;
				for (int xx = x0; xx < x1; xx++, row += 4) {
					sum[0] += row[0];
					sum[1] += row[1];
					sum[2] += row[2];
					sum[3] += row[3];
					n++;
				}
			}
			uint8_t *o = out + ((size_t)y * dw + x) * 4;
			for (int c = 0; c < 4; c++)
				o[c] = (uint8_t)(n ? sum[c] / n : 0);
		}
	}
	return out;
}
