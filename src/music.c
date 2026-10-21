#include "music.h"

#include "at9.h"
#include "jobs.h"
#include "log.h"

#include <malloc.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_BYTES (24 * 1024 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int started, stopping;
static char requested[512];   // what should be playing ("" = nothing)
static int request_seq;       // bumped on every play/stop request
static char current[512];     // what the thread is working on
static MusicState state;
static int library_ready;

static int request_changed(int seq) {
	pthread_mutex_lock(&lock);
	int changed = seq != request_seq || stopping;
	pthread_mutex_unlock(&lock);
	return changed;
}

static void set_state(MusicState s) {
	pthread_mutex_lock(&lock);
	state = s;
	pthread_mutex_unlock(&lock);
}

// Reads the whole file into a buffer aligned and padded as the decoder needs.
static uint8_t *read_aligned(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = NULL;
	if (n > 0 && n <= MAX_FILE_BYTES) {
		buf = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP((size_t)n + SCE_AUDIODEC_AT9_MAX_ES_SIZE));
		if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		if (buf)
			memset(buf + n, 0, SCE_AUDIODEC_AT9_MAX_ES_SIZE);
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static int play_file(const char *path, int seq) {
	size_t size;
	uint8_t *file = read_aligned(path, &size);
	At9Info at9;
	if (!file || at9_parse(file, size, &at9) != 0) {
		log_write("music: %s is not a playable ATRAC9 file", path);
		free(file);
		return -1;
	}
	if (!library_ready) {
		SceAudiodecInitParam init;
		memset(&init, 0, sizeof(init));
		init.size = sizeof(init.at9);
		init.at9.size = sizeof(init.at9);
		init.at9.totalCh = 2;
		int rc = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AT9, &init);
		if (rc < 0) {
			log_write("music: sceAudiodecInitLibrary failed: 0x%08X", (unsigned)rc);
			free(file);
			return -1;
		}
		library_ready = 1;
	}

	SceAudiodecInfo info;
	SceAudiodecCtrl ctrl;
	memset(&info, 0, sizeof(info));
	memset(&ctrl, 0, sizeof(ctrl));
	info.size = sizeof(info.at9);
	memcpy(info.at9.configData, at9.config, 4);
	ctrl.size = sizeof(ctrl);
	ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
	ctrl.pInfo = &info;
	int rc = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AT9);
	if (rc < 0) {
		log_write("music: sceAudiodecCreateDecoder failed: 0x%08X", (unsigned)rc);
		free(file);
		return -1;
	}

	int channels = (int)info.at9.ch ? (int)info.at9.ch : at9.channels;
	int grain = (int)(ctrl.maxPcmSize / (unsigned)channels / sizeof(int16_t));
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, (int)info.at9.samplingRate,
	                               channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
	uint8_t *pcm[2] = {memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize)),
	                   memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize))};
	if (port < 0 || !pcm[0] || !pcm[1]) {
		log_write("music: could not open audio output: 0x%08X", (unsigned)port);
		rc = -1;
	} else {
		int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
		sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volume);
		set_state(MUSIC_PLAYING);
		uint32_t start = at9.data_offset, end = at9.data_offset + at9.data_size, pos = start;
		int which = 0;
		rc = 0;
		while (!request_changed(seq)) {
			if (pos + ctrl.maxEsSize > end)
				pos = start; // loop the preview
			ctrl.pEs = file + pos;
			ctrl.pPcm = pcm[which];
			int r = sceAudiodecDecode(&ctrl);
			if (r < 0 || ctrl.inputEsSize == 0) {
				log_write("music: decode failed at %u: 0x%08X", (unsigned)pos, (unsigned)r);
				rc = -1;
				break;
			}
			pos += ctrl.inputEsSize;
			sceAudioOutOutput(port, pcm[which]);
			which ^= 1;
		}
		sceAudioOutOutput(port, NULL); // wait for the last buffer to finish
	}
	if (port >= 0)
		sceAudioOutReleasePort(port);
	sceAudiodecDeleteDecoder(&ctrl);
	free(pcm[0]);
	free(pcm[1]);
	free(file);
	return rc;
}

static void *player(void *arg) {
	(void)arg;
	int handled = 0;
	pthread_mutex_lock(&lock);
	while (!stopping) {
		if (handled == request_seq) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		handled = request_seq;
		snprintf(current, sizeof(current), "%s", requested);
		state = current[0] ? MUSIC_LOADING : MUSIC_IDLE;
		pthread_mutex_unlock(&lock);
		int rc = current[0] ? play_file(current, handled) : 0;
		pthread_mutex_lock(&lock);
		if (handled == request_seq)
			state = rc == 0 ? MUSIC_IDLE : MUSIC_ERROR;
	}
	pthread_mutex_unlock(&lock);
	return NULL;
}

void music_init(void) {
	started = jobs_spawn_thread(player, NULL, &thread) == 0;
}

void music_shutdown(void) {
	pthread_mutex_lock(&lock);
	stopping = 1;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	if (started)
		pthread_join(thread, NULL);
	if (library_ready)
		sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AT9);
}

static void request(const char *path) {
	pthread_mutex_lock(&lock);
	snprintf(requested, sizeof(requested), "%s", path);
	request_seq++;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void music_play(const char *path) {
	request(path);
}

void music_stop(void) {
	request("");
}

MusicState music_state(const char *path) {
	pthread_mutex_lock(&lock);
	MusicState s = strcmp(requested, path) == 0 && path[0] ? (strcmp(current, path) == 0 ? state : MUSIC_LOADING)
	                                                       : MUSIC_IDLE;
	pthread_mutex_unlock(&lock);
	return s;
}
