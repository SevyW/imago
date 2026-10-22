#include "imageio.h"

#include "fs.h"

#include <png.h>
#include <stdio.h>
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

int image_load_any(const char *path, uint8_t **pixels, int *width, int *height, int min_width, int min_height) {
	unsigned char magic[4] = {0};
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	size_t n = fread(magic, 1, sizeof(magic), f);
	fclose(f);
	if (n >= 2 && magic[0] == 0xFF && magic[1] == 0xD8)
		return image_load_jpeg(path, pixels, width, height, min_width, min_height);
	if (n >= 4 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
		return image_load_png(path, pixels, width, height);
	return -1;
}

// --- palette PNGs -------------------------------------------------------------------
// Median cut over a 5-bit-per-channel histogram picks the palette; pixels are
// then mapped with Floyd-Steinberg dithering through a nearest-colour cache.

#define HIST_BITS 5
#define HIST_SIZE (1 << (3 * HIST_BITS))

typedef struct {
	int lo[3], hi[3]; // inclusive bounds in histogram space
	uint32_t count;
} Box;

static int hist_index(int r, int g, int b) {
	return (r << (2 * HIST_BITS)) | (g << HIST_BITS) | b;
}

// Shrinks a box to the populated part of the histogram and counts its pixels.
static void box_fit(Box *box, const uint32_t *hist) {
	int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
	uint32_t count = 0;
	for (int r = box->lo[0]; r <= box->hi[0]; r++)
		for (int g = box->lo[1]; g <= box->hi[1]; g++)
			for (int b = box->lo[2]; b <= box->hi[2]; b++) {
				uint32_t c = hist[hist_index(r, g, b)];
				if (!c)
					continue;
				count += c;
				int v[3] = {r, g, b};
				for (int k = 0; k < 3; k++) {
					if (v[k] < lo[k])
						lo[k] = v[k];
					if (v[k] > hi[k])
						hi[k] = v[k];
				}
			}
	box->count = count;
	if (count)
		for (int k = 0; k < 3; k++) {
			box->lo[k] = lo[k];
			box->hi[k] = hi[k];
		}
}

// Splits a box at the median of its longest side.
static Box box_split(Box *b, int axis, const uint32_t *hist) {
	uint32_t half = b->count / 2, seen = 0;
	int cut = b->lo[axis];
	int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
	for (int v = b->lo[axis]; v < b->hi[axis]; v++) {
		for (int x = b->lo[a1]; x <= b->hi[a1]; x++)
			for (int y = b->lo[a2]; y <= b->hi[a2]; y++) {
				int c[3];
				c[axis] = v;
				c[a1] = x;
				c[a2] = y;
				seen += hist[hist_index(c[0], c[1], c[2])];
			}
		cut = v;
		if (seen >= half)
			break;
	}
	Box upper = *b;
	b->hi[axis] = cut;
	upper.lo[axis] = cut + 1;
	box_fit(b, hist);
	box_fit(&upper, hist);
	return upper;
}

static int build_palette(const uint8_t *rgba, size_t pixels, uint8_t *palette) {
	uint32_t *hist = calloc(HIST_SIZE, sizeof(uint32_t));
	uint32_t *sums = calloc((size_t)HIST_SIZE * 3, sizeof(uint32_t));
	Box *boxes = malloc(256 * sizeof(Box));
	int n = 0;
	if (hist && sums && boxes) {
		for (size_t i = 0; i < pixels; i++) {
			const uint8_t *p = rgba + i * 4;
			int h = hist_index(p[0] >> 3, p[1] >> 3, p[2] >> 3);
			hist[h]++;
			sums[h * 3] += p[0];
			sums[h * 3 + 1] += p[1];
			sums[h * 3 + 2] += p[2];
		}
		boxes[0] = (Box){{0, 0, 0}, {31, 31, 31}, 0};
		box_fit(&boxes[0], hist);
		n = 1;
		while (n < 256) {
			// Split the box with the most pixels times its longest side.
			int best = -1, axis = 0;
			double best_score = 0;
			for (int i = 0; i < n; i++)
				for (int k = 0; k < 3; k++) {
					int len = boxes[i].hi[k] - boxes[i].lo[k];
					double score = (double)boxes[i].count * len;
					if (len > 0 && score > best_score) {
						best_score = score;
						best = i;
						axis = k;
					}
				}
			if (best < 0)
				break; // every box is a single histogram cell
			boxes[n] = box_split(&boxes[best], axis, hist);
			n++;
		}
		for (int i = 0; i < n; i++) {
			uint64_t total = 0, acc[3] = {0, 0, 0};
			for (int r = boxes[i].lo[0]; r <= boxes[i].hi[0]; r++)
				for (int g = boxes[i].lo[1]; g <= boxes[i].hi[1]; g++)
					for (int b = boxes[i].lo[2]; b <= boxes[i].hi[2]; b++) {
						int h = hist_index(r, g, b);
						total += hist[h];
						for (int k = 0; k < 3; k++)
							acc[k] += sums[h * 3 + k];
					}
			for (int k = 0; k < 3; k++)
				palette[i * 3 + k] = (uint8_t)(total ? acc[k] / total : 0);
		}
	}
	free(hist);
	free(sums);
	free(boxes);
	return n;
}

static int nearest_colour(const uint8_t *palette, int n, int r, int g, int b) {
	int best = 0, best_d = 1 << 30;
	for (int i = 0; i < n; i++) {
		int dr = r - palette[i * 3], dg = g - palette[i * 3 + 1], db = b - palette[i * 3 + 2];
		int d = dr * dr * 3 + dg * dg * 4 + db * db * 2; // rough perceptual weights
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

static int clamp255(int v) {
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

int image_save_png_indexed(const char *path, const uint8_t *rgba, int width, int height) {
	if (width <= 0 || height <= 0)
		return -1;
	uint8_t palette[256 * 3];
	memset(palette, 0, sizeof(palette));
	int colours = build_palette(rgba, (size_t)width * height, palette);
	uint8_t *indices = malloc((size_t)width * height);
	int16_t *cache = malloc((1 << 18) * sizeof(int16_t));     // 6 bits per channel
	int *err = calloc((size_t)(width + 2) * 6, sizeof(int)); // this row and the next
	int rc = -1;
	if (colours > 0 && indices && cache && err) {
		memset(cache, 0xFF, (1 << 18) * sizeof(int16_t));
		int *cur = err, *next = err + (width + 2) * 3;
		for (int y = 0; y < height; y++) {
			memset(next, 0, (size_t)(width + 2) * 3 * sizeof(int));
			for (int x = 0; x < width; x++) {
				const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
				int *e = cur + (x + 1) * 3;
				int c[3];
				for (int k = 0; k < 3; k++)
					c[k] = clamp255(p[k] + e[k] / 16);
				int key = ((c[0] >> 2) << 12) | ((c[1] >> 2) << 6) | (c[2] >> 2);
				if (cache[key] < 0)
					cache[key] = (int16_t)nearest_colour(palette, colours, c[0], c[1], c[2]);
				int idx = cache[key];
				indices[(size_t)y * width + x] = (uint8_t)idx;
				for (int k = 0; k < 3; k++) {
					int d = c[k] - palette[idx * 3 + k];
					e[3 + k] += d * 7;              // right
					next[x * 3 + k] += d * 3;       // below left
					next[(x + 1) * 3 + k] += d * 5; // below
					next[(x + 2) * 3 + k] += d;     // below right
				}
			}
			int *swap = cur;
			cur = next;
			next = swap;
		}
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		image.width = (png_uint_32)width;
		image.height = (png_uint_32)height;
		image.format = PNG_FORMAT_RGB_COLORMAP;
		// A full palette keeps the file 8-bit even for pictures with few colours
		// (libpng would otherwise pick 1, 2 or 4 bits); unused entries are black.
		image.colormap_entries = 256;
		rc = png_image_write_to_file(&image, path, 0, indices, 0, palette) ? 0 : -1;
		png_image_free(&image);
	}
	free(indices);
	free(cache);
	free(err);
	return rc;
}

int image_png_size(const char *path, int *width, int *height) {
	unsigned char h[24];
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	size_t n = fread(h, 1, sizeof(h), f);
	fclose(f);
	if (n != sizeof(h) || memcmp(h, "\x89PNG\r\n\x1a\n", 8) != 0 || memcmp(h + 12, "IHDR", 4) != 0)
		return -1;
	*width = h[16] << 24 | h[17] << 16 | h[18] << 8 | h[19];
	*height = h[20] << 24 | h[21] << 16 | h[22] << 8 | h[23];
	return *width > 0 && *height > 0 ? 0 : -1;
}

// A palette image from indices (one per pixel) and an RGBA palette. The
// palette is padded to 256 entries so the file is always 8-bit.
int image_save_png_palette(const char *path, const uint8_t *indices, int width, int height, const uint8_t *palette,
                           int colors) {
	uint8_t full[256 * 4];
	memset(full, 0, sizeof(full));
	memcpy(full, palette, (size_t)(colors < 256 ? colors : 256) * 4);
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = (png_uint_32)width;
	image.height = (png_uint_32)height;
	image.format = PNG_FORMAT_RGBA_COLORMAP;
	image.colormap_entries = 256;
	int ok = png_image_write_to_file(&image, path, 0, indices, 0, full);
	png_image_free(&image);
	return ok ? 0 : -1;
}

static void sample_bilinear(const uint8_t *src, int sw, int sh, double fx, double fy, uint8_t *out) {
	fx -= 0.5;
	fy -= 0.5;
	if (fx < 0)
		fx = 0;
	if (fy < 0)
		fy = 0;
	int x0 = (int)fx, y0 = (int)fy;
	if (x0 > sw - 1)
		x0 = sw - 1;
	if (y0 > sh - 1)
		y0 = sh - 1;
	int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
	double tx = fx - x0, ty = fy - y0;
	if (tx > 1.0)
		tx = 1.0;
	if (ty > 1.0)
		ty = 1.0;
	const uint8_t *a = src + ((size_t)y0 * sw + x0) * 4, *b = src + ((size_t)y0 * sw + x1) * 4;
	const uint8_t *c = src + ((size_t)y1 * sw + x0) * 4, *d = src + ((size_t)y1 * sw + x1) * 4;
	for (int k = 0; k < 4; k++) {
		double top = a[k] + (b[k] - a[k]) * tx, bottom = c[k] + (d[k] - c[k]) * tx;
		out[k] = (uint8_t)(top + (bottom - top) * ty + 0.5);
	}
}

static void clamp_span(int *lo, int *hi, int limit) {
	if (*lo < 0)
		*lo = 0;
	if (*hi > limit)
		*hi = limit;
	if (*lo > limit - 1)
		*lo = limit - 1;
	if (*hi <= *lo)
		*hi = *lo + 1;
}

uint8_t *image_resize_region(const uint8_t *src, int sw, int sh, double rx, double ry, double rw, double rh, int dw,
                             int dh) {
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || rw <= 0 || rh <= 0)
		return NULL;
	uint8_t *out = malloc((size_t)dw * (size_t)dh * 4);
	if (!out)
		return NULL;
	double step_x = rw / dw, step_y = rh / dh;
	int enlarge = step_x < 1.0 || step_y < 1.0;
	for (int y = 0; y < dh; y++) {
		int y0 = (int)(ry + y * step_y), y1 = (int)(ry + (y + 1) * step_y);
		clamp_span(&y0, &y1, sh);
		for (int x = 0; x < dw; x++) {
			uint8_t *o = out + ((size_t)y * dw + x) * 4;
			if (enlarge) {
				sample_bilinear(src, sw, sh, rx + (x + 0.5) * step_x, ry + (y + 0.5) * step_y, o);
				continue;
			}
			int x0 = (int)(rx + x * step_x), x1 = (int)(rx + (x + 1) * step_x);
			clamp_span(&x0, &x1, sw);
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
			for (int c = 0; c < 4; c++)
				o[c] = (uint8_t)(n ? sum[c] / n : 0);
		}
	}
	return out;
}
