// JPEG decoding for pictures from the memory card (kept apart from imageio.c
// so the host tests don't need libjpeg).
#include "imageio.h"

#include "fs.h"

#include <stdio.h> // before jpeglib.h, which uses FILE
#include <stdlib.h>

#include <jpeglib.h>
#include <setjmp.h>

// libjpeg's default error handler exits the process; jump back out instead.
typedef struct {
	struct jpeg_error_mgr base;
	jmp_buf jump;
} JpegError;

static void jpeg_fail(j_common_ptr info) {
	longjmp(((JpegError *)info->err)->jump, 1);
}

int image_load_jpeg(const char *path, uint8_t **pixels, int *width, int *height, int min_width, int min_height) {
	char *data;
	size_t size;
	if (fs_read_file(path, &data, &size) != 0)
		return -1;
	struct jpeg_decompress_struct info;
	JpegError err;
	uint8_t *volatile buffer = NULL;
	info.err = jpeg_std_error(&err.base);
	err.base.error_exit = jpeg_fail;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&info);
		free(buffer);
		free(data);
		return -1;
	}
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, (unsigned char *)data, (unsigned long)size);
	jpeg_read_header(&info, TRUE);
	// Decode big photos at 1/2, 1/4 or 1/8 scale while still covering the target.
	unsigned denom = 1;
	while (denom < 8 && info.image_width / (denom * 2) >= (unsigned)min_width &&
	       info.image_height / (denom * 2) >= (unsigned)min_height)
		denom *= 2;
	info.scale_num = 1;
	info.scale_denom = denom;
	info.out_color_space = JCS_EXT_RGBA;
	jpeg_start_decompress(&info);
	if (info.output_width == 0 || info.output_height == 0 || info.output_width > 4096 || info.output_height > 4096)
		longjmp(err.jump, 1);
	size_t stride = (size_t)info.output_width * 4;
	buffer = malloc(stride * info.output_height);
	if (!buffer)
		longjmp(err.jump, 1);
	while (info.output_scanline < info.output_height) {
		JSAMPROW rows[1] = {buffer + stride * info.output_scanline};
		jpeg_read_scanlines(&info, rows, 1);
	}
	*width = (int)info.output_width;
	*height = (int)info.output_height;
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	free(data);
	*pixels = buffer;
	return 0;
}

// JPEGs are decoded at 1/2, 1/4 or 1/8 scale until they fit max_pixels.
static int load_jpeg_limited(const char *path, uint8_t **pixels, int *width, int *height, long max_pixels) {
	char *data;
	size_t size;
	if (fs_read_file(path, &data, &size) != 0)
		return -1;
	struct jpeg_decompress_struct info;
	JpegError err;
	uint8_t *volatile buffer = NULL;
	info.err = jpeg_std_error(&err.base);
	err.base.error_exit = jpeg_fail;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&info);
		free(buffer);
		free(data);
		return -1;
	}
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, (unsigned char *)data, (unsigned long)size);
	jpeg_read_header(&info, TRUE);
	info.out_color_space = JCS_EXT_RGBA;
	info.scale_num = 1;
	info.scale_denom = 1;
	while (info.scale_denom < 8 &&
	       (long long)(info.image_width / info.scale_denom) * (info.image_height / info.scale_denom) > max_pixels)
		info.scale_denom *= 2;
	jpeg_start_decompress(&info);
	if ((long long)info.output_width * info.output_height > max_pixels || info.output_width == 0)
		longjmp(err.jump, 1);
	size_t stride = (size_t)info.output_width * 4;
	buffer = malloc(stride * info.output_height);
	if (!buffer)
		longjmp(err.jump, 1);
	while (info.output_scanline < info.output_height) {
		JSAMPROW rows[1] = {buffer + stride * info.output_scanline};
		jpeg_read_scanlines(&info, rows, 1);
	}
	*pixels = buffer;
	*width = (int)info.output_width;
	*height = (int)info.output_height;
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	free(data);
	return 0;
}

int image_load_limited(const char *path, uint8_t **pixels, int *width, int *height, long max_pixels) {
	int w, h;
	if (image_png_size(path, &w, &h) == 0) // a PNG: refuse one that's too big rather than decode it
		return (long long)w * h <= max_pixels ? image_load_png(path, pixels, width, height) : -1;
	return load_jpeg_limited(path, pixels, width, height, max_pixels);
}
