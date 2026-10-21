#include "log.h"

#include "config.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define LOG_PATH DATA_DIR "/log.txt"
#define MAX_LOG_BYTES (256 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(void) {
	// Start fresh once the log grows large, keeping the last session readable.
	FILE *f = fopen(LOG_PATH, "rb");
	long size = 0;
	if (f) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fclose(f);
	}
	if (size > MAX_LOG_BYTES)
		remove(LOG_PATH);
	log_write("---- Imago started ----");
}

void log_write(const char *fmt, ...) {
	pthread_mutex_lock(&lock);
	FILE *f = fopen(LOG_PATH, "ab");
	if (f) {
		time_t now = time(NULL);
		struct tm *t = localtime(&now);
		if (t)
			fprintf(f, "%02d:%02d:%02d ", t->tm_hour, t->tm_min, t->tm_sec);
		va_list args;
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);
		fputc('\n', f);
		fclose(f);
	}
	pthread_mutex_unlock(&lock);
}
