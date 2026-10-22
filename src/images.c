#include "images.h"

#include "fs.h"
#include "jobs.h"
#include "log.h"
#include "net.h"
#include "psvt.h"
#include "ui.h"

#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#define CACHE_DIR "ux0:data/Imago/cache"
#define SLOTS 160
#define WORKERS 3
#define STALE_FRAMES 30     // queued requests not drawn for this long are dropped
#define RETRY_US 8000000ULL // failed downloads are retried after 8 s
#define FADE_US 220000.0f
#define UPLOAD_PIXELS_PER_FRAME (1200 * 1024) // texture uploads per frame, so a burst never stalls one

// QUEUED -> LOADING (a worker decodes) -> DECODED (pixels waiting for the main
// thread) -> READY (a texture), or FAILED.
typedef enum { SLOT_FREE, SLOT_QUEUED, SLOT_LOADING, SLOT_DECODED, SLOT_READY, SLOT_FAILED } SlotState;

typedef struct {
	char key[80];
	char url[512];  // empty for local files
	char path[384]; // cache file, or the local source file
	int max_width;  // local files: decode at most about this wide (0 = full size)
	SlotState state;
	uint8_t *pixels; // DECODED: RGBA waiting to become a texture
	int pixels_w, pixels_h;
	vita2d_texture *tex;
	unsigned last_used;
	unsigned long long ready_at;
} Slot;

static Slot slots[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t workers[WORKERS];
static int worker_count;
static int stopping;
static unsigned frame;

static vita2d_texture *to_free[SLOTS];
static int to_free_count;

static unsigned long long now_us(void) {
	return sceKernelGetProcessTimeWide();
}

// Workers only decode into plain memory; textures are created on the main
// thread (images_frame), between frames. libgxm isn't safe to call from other
// threads while the main thread is drawing: creating textures in the workers
// occasionally crashed the GPU, most often right after launch when many
// cached thumbnails decode at once.
typedef struct {
	uint8_t *data; // tightly packed RGBA
	int w, h;
} Pixels;

// vita2d's own JPEG loader rejects files that don't start with an APP0/APP1
// segment (wsrv.nl's optimised JPEGs start with DQT) and its error handler
// exits the process on corrupt data, so decode with libjpeg directly.
typedef struct {
	struct jpeg_error_mgr base;
	jmp_buf jump;
} JpegError;

static void jpeg_fail(j_common_ptr info) {
	longjmp(((JpegError *)info->err)->jump, 1);
}

// max_width 0 keeps photos at least screen-sized; otherwise they're decoded
// as small as possible while staying at least max_width wide.
static int decode_jpeg(const unsigned char *data, size_t size, int max_width, Pixels *out) {
	struct jpeg_decompress_struct info;
	JpegError err;
	uint8_t *volatile buffer = NULL;
	info.err = jpeg_std_error(&err.base);
	err.base.error_exit = jpeg_fail;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&info);
		free(buffer);
		return -1;
	}
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, data, (unsigned long)size);
	jpeg_read_header(&info, TRUE);
	// Photos from the memory card can be huge: decode them at 1/2, 1/4 or 1/8
	// scale, as long as the result still covers what's needed.
	unsigned min_w = max_width > 0 ? (unsigned)max_width : SCREEN_W, min_h = max_width > 0 ? 1 : SCREEN_H;
	unsigned denom = 1;
	while (denom < 8 && info.image_width / (denom * 2) >= min_w && info.image_height / (denom * 2) >= min_h)
		denom *= 2;
	info.scale_num = 1;
	info.scale_denom = denom;
	info.out_color_space = JCS_EXT_RGBA; // matches vita2d's default ABGR texture layout
	info.dct_method = JDCT_IFAST;
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
	out->data = buffer;
	out->w = (int)info.output_width;
	out->h = (int)info.output_height;
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	return 0;
}

// Averages factor x factor blocks of an RGBA image, in place.
static void shrink(Pixels *px, int factor) {
	int dw = px->w / factor, dh = px->h / factor;
	unsigned n = (unsigned)(factor * factor);
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			unsigned sum[4] = {0, 0, 0, 0};
			for (int yy = 0; yy < factor; yy++) {
				const uint8_t *row = px->data + ((size_t)(y * factor + yy) * px->w + (size_t)x * factor) * 4;
				for (int xx = 0; xx < factor * 4; xx++)
					sum[xx & 3] += row[xx];
			}
			uint8_t *o = px->data + ((size_t)y * dw + x) * 4; // never ahead of what's still to be read
			for (int c = 0; c < 4; c++)
				o[c] = (uint8_t)(sum[c] / n);
		}
	px->w = dw;
	px->h = dh;
}

// vita2d_load_PNG_buffer() takes no length and reads past the end of a
// truncated file (theme thumbnails come from untrusted zips). libpng's
// simplified API is bounded and keeps its error handling (setjmp) inside
// libpng, so no jmp_buf ever crosses the library boundary.
static int decode_png(const unsigned char *data, size_t size, int max_width, Pixels *out) {
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, data, size))
		return -1;
	image.format = PNG_FORMAT_RGBA; // vita2d's default texture layout
	int rc = -1;
	if (image.width > 0 && image.height > 0 && image.width <= 4096 && image.height <= 4096) {
		uint8_t *buffer = malloc(PNG_IMAGE_SIZE(image));
		if (buffer && png_image_finish_read(&image, NULL, buffer, 0, NULL)) {
			*out = (Pixels){buffer, (int)image.width, (int)image.height};
			// A big picture shown small (grid cards): keep only what's needed.
			int factor = max_width > 0 ? out->w / max_width : 1;
			if (factor >= 2)
				shrink(out, factor);
			rc = 0;
		} else {
			free(buffer);
		}
	}
	if (rc != 0)
		log_write("image: PNG error: %s", image.message[0] ? image.message : "unsupported image");
	png_image_free(&image);
	return rc;
}

static int decode(const unsigned char *data, size_t size, int max_width, Pixels *out) {
	if (size > 3 && data[0] == 0xFF && data[1] == 0xD8)
		return decode_jpeg(data, size, max_width, out);
	if (size > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		return decode_png(data, size, max_width, out);
	return -1;
}

static int load(Slot *job, NetClient *net, Pixels *out) {
	char *data = NULL;
	size_t size = 0;
	if (fs_read_file(job->path, &data, &size) == 0 && size > 0) {
		int rc = decode((unsigned char *)data, size, job->max_width, out);
		if (rc != 0)
			log_write("image: could not decode %s", job->path);
		free(data);
		if (rc == 0 || !job->url[0])
			return rc;
		remove(job->path); // corrupt cache entry: fetch it again
	}
	if (!job->url[0] || !net)
		return -1;
	if (net_get(net, job->url, &data, &size, 4 * 1024 * 1024, NULL, &stopping) != NET_OK) {
		// Query strings can carry keys: log the address without one.
		log_write("image: %.*s: %s", (int)strcspn(job->url, "?"), job->url, net_error(net));
		return -1;
	}
	int rc = decode((unsigned char *)data, size, job->max_width, out);
	if (rc == 0)
		fs_write_file(job->path, data, size);
	else
		log_write("image: could not decode %u bytes from %.*s", (unsigned)size, (int)strcspn(job->url, "?"), job->url);
	free(data);
	return rc;
}

static Slot *next_job(void) {
	Slot *best = NULL;
	for (int i = 0; i < SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state != SLOT_QUEUED)
			continue;
		if (frame - s->last_used > STALE_FRAMES) {
			s->state = SLOT_FREE; // scrolled away before we got to it
			continue;
		}
		if (!best || s->last_used > best->last_used)
			best = s; // most recently requested = currently on screen
	}
	return best;
}

static void *worker(void *arg) {
	(void)arg;
	NetClient *net = net_client_new();
	pthread_mutex_lock(&lock);
	while (!stopping) {
		Slot *s = next_job();
		if (!s) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		s->state = SLOT_LOADING;
		Slot job = *s; // work on a copy without holding the lock
		pthread_mutex_unlock(&lock);

		Pixels px = {NULL, 0, 0};
		int rc = load(&job, net, &px);

		pthread_mutex_lock(&lock);
		s->pixels = rc == 0 ? px.data : NULL;
		s->pixels_w = px.w;
		s->pixels_h = px.h;
		s->state = rc == 0 ? SLOT_DECODED : SLOT_FAILED;
		s->ready_at = now_us();
	}
	pthread_mutex_unlock(&lock);
	net_client_free(net);
	return NULL;
}

void images_init(void) {
	fs_mkdir_p(CACHE_DIR);
	for (int i = 0; i < WORKERS; i++)
		if (jobs_spawn_thread(worker, NULL, &workers[worker_count]) == 0)
			worker_count++;
}

void images_shutdown(void) {
	pthread_mutex_lock(&lock);
	stopping = 1;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < worker_count; i++)
		pthread_join(workers[i], NULL);
	vita2d_wait_rendering_done();
	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].tex)
			vita2d_free_texture(slots[i].tex);
		free(slots[i].pixels);
	}
	memset(slots, 0, sizeof(slots));
}

void images_frame(void) {
	pthread_mutex_lock(&lock);
	frame++;
	int count = to_free_count;
	vita2d_texture *batch[SLOTS];
	memcpy(batch, to_free, sizeof(batch[0]) * (size_t)count);
	to_free_count = 0;
	pthread_mutex_unlock(&lock);
	if (count) {
		vita2d_wait_rendering_done(); // the GPU may still sample them
		for (int i = 0; i < count; i++)
			vita2d_free_texture(batch[i]);
	}

	// Turn decoded pictures into textures, here on the main thread and before
	// this frame's drawing starts.
	int budget = UPLOAD_PIXELS_PER_FRAME;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < SLOTS && budget > 0; i++) {
		Slot *s = &slots[i];
		if (s->state != SLOT_DECODED)
			continue;
		vita2d_texture *tex = vita2d_create_empty_texture((unsigned)s->pixels_w, (unsigned)s->pixels_h);
		if (tex) {
			uint8_t *dst = vita2d_texture_get_datap(tex);
			unsigned stride = vita2d_texture_get_stride(tex);
			size_t row = (size_t)s->pixels_w * 4;
			for (int y = 0; y < s->pixels_h; y++)
				memcpy(dst + (size_t)y * stride, s->pixels + (size_t)y * row, row);
			vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		}
		free(s->pixels);
		s->pixels = NULL;
		s->tex = tex;
		s->state = tex ? SLOT_READY : SLOT_FAILED;
		s->ready_at = now_us();
		budget -= s->pixels_w * s->pixels_h;
	}
	pthread_mutex_unlock(&lock);
}

// Finds a slot to reuse. Called with the lock held.
static Slot *claim_slot(void) {
	Slot *victim = NULL;
	for (int i = 0; i < SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state == SLOT_FREE)
			return s;
		int evictable = (s->state == SLOT_READY || s->state == SLOT_FAILED || s->state == SLOT_QUEUED) &&
		                frame - s->last_used > 1;
		if (evictable && (!victim || s->last_used < victim->last_used))
			victim = s;
	}
	if (victim && victim->tex && to_free_count < SLOTS)
		to_free[to_free_count++] = victim->tex;
	else if (victim && victim->tex)
		return NULL; // free list full; try again next frame
	return victim;
}

static ImageRef request(const char *key, const char *url, const char *path, int max_width) {
	ImageRef ref = {NULL, 0.0f, 0};
	pthread_mutex_lock(&lock);
	Slot *s = NULL;
	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].state != SLOT_FREE && strcmp(slots[i].key, key) == 0) {
			s = &slots[i];
			break;
		}
	}
	if (s && s->state == SLOT_FAILED && s->url[0] && now_us() - s->ready_at > RETRY_US) {
		s->state = SLOT_QUEUED;
		pthread_cond_signal(&wake);
	}
	if (!s && (s = claim_slot())) {
		memset(s, 0, sizeof(*s));
		snprintf(s->key, sizeof(s->key), "%s", key);
		snprintf(s->url, sizeof(s->url), "%s", url ? url : "");
		snprintf(s->path, sizeof(s->path), "%s", path);
		s->max_width = max_width;
		s->state = SLOT_QUEUED;
		pthread_cond_signal(&wake);
	}
	if (s) {
		s->last_used = frame;
		if (s->state == SLOT_READY) {
			ref.tex = s->tex;
			float t = (float)(now_us() - s->ready_at) / FADE_US;
			ref.alpha = t > 1.0f ? 1.0f : t;
		}
		ref.failed = s->state == SLOT_FAILED;
	}
	pthread_mutex_unlock(&lock);
	return ref;
}

ImageRef images_remote(const char *hash, int width) {
	char key[80], url[256], path[256];
	snprintf(key, sizeof(key), "%s@%d", hash, width);
	psvt_preview_url(url, sizeof(url), hash, width);
	snprintf(path, sizeof(path), CACHE_DIR "/%s_%d.jpg", hash, width);
	return request(key, url, path, 0);
}

// Paths and URLs can be long; a hash keeps keys unique and short.
static unsigned long hash_text(const char *text) {
	unsigned long h = 5381;
	for (const char *p = text; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return h;
}

ImageRef images_url(const char *url, int max_width) {
	char key[80], path[256];
	unsigned long h = hash_text(url);
	snprintf(key, sizeof(key), "url%d:%08lx:%.50s", max_width, h, url + (strlen(url) > 50 ? strlen(url) - 50 : 0));
	snprintf(path, sizeof(path), CACHE_DIR "/u%08lx_%u.img", h, (unsigned)strlen(url));
	return request(key, url, path, max_width);
}

ImageRef images_file_sized(const char *path, int max_width) {
	char key[80];
	unsigned long h = hash_text(path);
	snprintf(key, sizeof(key), "file%d:%08lx:%.50s", max_width, h, path + (strlen(path) > 50 ? strlen(path) - 50 : 0));
	return request(key, NULL, path, max_width);
}

ImageRef images_file(const char *path) {
	return images_file_sized(path, 0);
}

void images_forget(const char *path) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state == SLOT_FREE || s->url[0] || strcmp(s->path, path) != 0)
			continue;
		if (s->state == SLOT_LOADING) {
			s->key[0] = '\0'; // orphaned: the worker's result is evicted later
		} else if (!s->tex || to_free_count < SLOTS) {
			if (s->tex)
				to_free[to_free_count++] = s->tex;
			free(s->pixels);
			memset(s, 0, sizeof(*s));
		}
	}
	pthread_mutex_unlock(&lock);
}

void images_clear_disk_cache(void) {
	fs_rm_rf(CACHE_DIR);
	fs_mkdir_p(CACHE_DIR);
}

long long images_disk_cache_size(void) {
	return fs_tree_size(CACHE_DIR);
}
