// Asynchronous image cache: remote previews (via the wsrv.nl resizer, cached
// on the memory card) and local PNG/JPEG files, decoded into textures by
// worker threads. Only call from the main thread.
#pragma once

#include <vita2d.h>

typedef struct {
	vita2d_texture *tex; // NULL while loading
	float alpha;         // 0..1 fade-in since the texture became ready
	int failed;
} ImageRef;

void images_init(void);
void images_shutdown(void);
// Call once per frame before drawing; frees evicted textures safely.
void images_frame(void);

ImageRef images_remote(const char *hash, int width);
ImageRef images_file(const char *path);
// A local picture shown small (grid cards): decoded at about max_width wide
// instead of full size, to keep texture memory down.
ImageRef images_file_sized(const char *path, int max_width);
#define IMAGES_CARD_WIDTH 256

void images_clear_disk_cache(void);
long long images_disk_cache_size(void);
