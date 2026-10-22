#include "qrscan.h"

#include "jobs.h"
#include "log.h"
#include "quirc.h"

#include <psp2/camera.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The camera writes ABGR frames straight into a texture, which is also the
// preview. Whenever the decoder thread is idle, the main thread hands it the
// frame's brightness, so decoding never stalls the picture.
static vita2d_texture *frame_tex;
static int open_device = -1, has_frame;
static struct quirc *decoder;

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int busy, stopping, found;
static char result[QUIRC_MAX_PAYLOAD + 1];

static void *decode_thread(void *arg) {
	(void)arg;
	pthread_mutex_lock(&lock);
	while (!stopping) {
		if (!busy) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		pthread_mutex_unlock(&lock);
		quirc_end(decoder);
		char text[sizeof(result)] = "";
		for (int i = 0, n = quirc_count(decoder); i < n && !text[0]; i++) {
			struct quirc_code code;
			struct quirc_data data;
			quirc_extract(decoder, i, &code);
			quirc_decode_error_t e = quirc_decode(&code, &data);
			if (e == QUIRC_ERROR_DATA_ECC) {
				quirc_flip(&code); // seen in a mirror
				e = quirc_decode(&code, &data);
			}
			if (e != QUIRC_SUCCESS)
				continue;
			snprintf(text, sizeof(text), "%.*s", data.payload_len, (const char *)data.payload);
		}
		pthread_mutex_lock(&lock);
		if (text[0] && !found) {
			memcpy(result, text, sizeof(result));
			found = 1;
		}
		busy = 0;
	}
	pthread_mutex_unlock(&lock);
	return NULL;
}

int qrscan_open(char *err, size_t err_size) {
	qrscan_close();
	frame_tex = vita2d_create_empty_texture_format(QRSCAN_W, QRSCAN_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	decoder = quirc_new();
	if (!frame_tex || !decoder || quirc_resize(decoder, QRSCAN_W, QRSCAN_H) < 0) {
		snprintf(err, err_size, "Not enough memory for the camera.");
		qrscan_close();
		return -1;
	}
	SceCameraInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	info.priority = SCE_CAMERA_PRIORITY_SHARE;
	info.format = SCE_CAMERA_FORMAT_ABGR;
	info.resolution = SCE_CAMERA_RESOLUTION_640_480;
	info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
	info.range = 1;
	info.pitch = (uint16_t)(vita2d_texture_get_stride(frame_tex) / 4 - QRSCAN_W);
	info.sizeIBase = vita2d_texture_get_stride(frame_tex) * QRSCAN_H;
	info.pIBase = vita2d_texture_get_datap(frame_tex);
	int rc = sceCameraOpen(SCE_CAMERA_DEVICE_BACK, &info);
	if (rc >= 0) {
		rc = sceCameraStart(SCE_CAMERA_DEVICE_BACK);
		if (rc < 0)
			sceCameraClose(SCE_CAMERA_DEVICE_BACK);
	}
	if (rc < 0) {
		log_write("qr: camera 0x%08X", (unsigned)rc);
		snprintf(err, err_size, "The camera couldn't be started (error 0x%08X).", (unsigned)rc);
		qrscan_close();
		return -1;
	}
	open_device = SCE_CAMERA_DEVICE_BACK;
	stopping = busy = found = 0;
	if (jobs_spawn_thread(decode_thread, NULL, &thread) != 0) {
		snprintf(err, err_size, "The camera couldn't be started.");
		qrscan_close();
		return -1;
	}
	return 0;
}

void qrscan_close(void) {
	if (open_device >= 0) {
		pthread_mutex_lock(&lock);
		stopping = 1;
		pthread_cond_signal(&wake);
		pthread_mutex_unlock(&lock);
		pthread_join(thread, NULL);
		sceCameraStop(open_device);
		sceCameraClose(open_device);
		open_device = -1;
	}
	if (decoder)
		quirc_destroy(decoder);
	decoder = NULL;
	if (frame_tex) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(frame_tex);
	}
	frame_tex = NULL;
	has_frame = 0;
}

int qrscan_poll(char *out, size_t size) {
	if (open_device < 0)
		return 0;
	SceCameraRead read;
	memset(&read, 0, sizeof(read));
	read.size = sizeof(read);
	if (sceCameraRead(open_device, &read) >= 0)
		has_frame = 1;

	pthread_mutex_lock(&lock);
	if (found) {
		snprintf(out, size, "%s", result);
		found = 0;
		pthread_mutex_unlock(&lock);
		return 1;
	}
	if (has_frame && !busy) {
		// Brightness of the ABGR frame for the decoder (green weighs most).
		int w, h;
		uint8_t *gray = quirc_begin(decoder, &w, &h);
		const uint8_t *px = vita2d_texture_get_datap(frame_tex);
		unsigned stride = vita2d_texture_get_stride(frame_tex);
		for (int y = 0; y < h; y++) {
			const uint8_t *row = px + (size_t)y * stride;
			for (int x = 0; x < w; x++, row += 4)
				*gray++ = (uint8_t)((row[0] * 77 + row[1] * 150 + row[2] * 29) >> 8);
		}
		busy = 1;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

vita2d_texture *qrscan_preview(void) {
	return has_frame ? frame_tex : NULL;
}
