// Host tests are built without libjpeg; pictures in the tests are PNGs.
#include "../src/imageio.h"

int image_load_jpeg(const char *path, uint8_t **pixels, int *width, int *height, int min_width, int min_height) {
	(void)path, (void)pixels, (void)width, (void)height, (void)min_width, (void)min_height;
	return -1;
}
