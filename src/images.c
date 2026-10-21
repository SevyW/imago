#include "images.h"

#include "fs.h"
#include "jobs.h"
#include "log.h"
#include "net.h"
#include "psvt.h"

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

typedef enum { SLOT_FREE, SLOT_QUEUED, SLOT_LOADING, SLOT_READY, SLOT_FAILED } SlotState;

typedef struct {
	char key[80];
	char url[256];  // empty for local files
	char path[256]; // cache file, or the local source file
	SlotState state;
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

static vita2d_texture *decode_jpeg(const unsigned char *data, size_t size) {
	struct jpeg_decompress_struct info;
	JpegError err;
	vita2d_texture *volatile tex = NULL;
	info.err = jpeg_std_error(&err.base);
	err.base.error_exit = jpeg_fail;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&info);
		if (tex)
			vita2d_free_texture(tex); // never drawn, so safe to free here
		return NULL;
	}
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, data, (unsigned long)size);
	jpeg_read_header(&info, TRUE);
	info.out_color_space = JCS_EXT_RGBA; // matches vita2d's default ABGR texture layout
	info.dct_method = JDCT_IFAST;
	jpeg_start_decompress(&info);
	if (info.output_width > 4096 || info.output_height > 4096)
		longjmp(err.jump, 1);
	tex = vita2d_create_empty_texture(info.output_width, info.output_height);
	if (!tex)
		longjmp(err.jump, 1);
	unsigned char *row = vita2d_texture_get_datap(tex);
	unsigned stride = vita2d_texture_get_stride(tex);
	while (info.output_scanline < info.output_height) {
		JSAMPROW rows[1] = {row};
		jpeg_read_scanlines(&info, rows, 1);
		row += stride;
	}
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	return tex;
}

// vita2d_load_PNG_buffer() takes no length and reads past the end of a
// truncated file (theme thumbnails come from untrusted zips). libpng's
// simplified API is bounded and keeps its error handling (setjmp) inside
// libpng, so no jmp_buf ever crosses the library boundary.
static vita2d_texture *decode_png(const unsigned char *data, size_t size) {
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, data, size))
		return NULL;
	vita2d_texture *tex = NULL;
	image.format = PNG_FORMAT_RGBA; // vita2d's default texture layout
	if (image.width > 0 && image.height > 0 && image.width <= 4096 && image.height <= 4096)
		tex = vita2d_create_empty_texture(image.width, image.height);
	// For 8-bit RGBA the row stride is given in bytes (one byte per component).
	if (tex && !png_image_finish_read(&image, NULL, vita2d_texture_get_datap(tex),
	                                  (png_int_32)vita2d_texture_get_stride(tex), NULL)) {
		vita2d_free_texture(tex); // never drawn, so safe to free here
		tex = NULL;
	}
	if (!tex)
		log_write("image: PNG error: %s", image.message[0] ? image.message : "unsupported image");
	png_image_free(&image);
	return tex;
}

static vita2d_texture *decode(const unsigned char *data, size_t size) {
	vita2d_texture *tex = NULL;
	if (size > 3 && data[0] == 0xFF && data[1] == 0xD8)
		tex = decode_jpeg(data, size);
	else if (size > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		tex = decode_png(data, size);
	if (tex)
		vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	return tex;
}

static vita2d_texture *load(Slot *job, NetClient *net) {
	char *data = NULL;
	size_t size = 0;
	if (fs_read_file(job->path, &data, &size) == 0 && size > 0) {
		vita2d_texture *tex = decode((unsigned char *)data, size);
		if (!tex)
			log_write("image: could not decode %s", job->path);
		free(data);
		if (tex || !job->url[0])
			return tex;
		remove(job->path); // corrupt cache entry: fetch it again
	}
	if (!job->url[0] || !net)
		return NULL;
	if (net_get(net, job->url, &data, &size, 4 * 1024 * 1024, NULL, &stopping) != NET_OK) {
		log_write("image: %s: %s", job->url, net_error(net));
		return NULL;
	}
	vita2d_texture *tex = decode((unsigned char *)data, size);
	if (tex)
		fs_write_file(job->path, data, size);
	else
		log_write("image: could not decode %u bytes from %s", (unsigned)size, job->url);
	free(data);
	return tex;
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

		vita2d_texture *tex = load(&job, net);

		pthread_mutex_lock(&lock);
		s->tex = tex;
		s->state = tex ? SLOT_READY : SLOT_FAILED;
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
	for (int i = 0; i < SLOTS; i++)
		if (slots[i].tex)
			vita2d_free_texture(slots[i].tex);
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

static ImageRef request(const char *key, const char *url, const char *path) {
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
	return request(key, url, path);
}

ImageRef images_file(const char *path) {
	char key[80];
	// Paths can be long; a hash keeps keys unique and short.
	unsigned long h = 5381;
	for (const char *p = path; *p; p++)
		h = h * 33 + (unsigned char)*p;
	snprintf(key, sizeof(key), "file:%08lx:%.50s", h, path + (strlen(path) > 50 ? strlen(path) - 50 : 0));
	return request(key, NULL, path);
}

void images_clear_disk_cache(void) {
	fs_rm_rf(CACHE_DIR);
	fs_mkdir_p(CACHE_DIR);
}

long long images_disk_cache_size(void) {
	return fs_tree_size(CACHE_DIR);
}
