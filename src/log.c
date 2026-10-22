#include "log.h"

#include <psp2/rtc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_LOG_BYTES (256 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(void) {
	// Sessions are appended; once the log is large it becomes the previous
	// log and a new one starts.
	FILE *f = fopen(LOG_PATH, "rb");
	long size = 0;
	if (f) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fclose(f);
	}
	if (size > MAX_LOG_BYTES) {
		remove(LOG_PREVIOUS_PATH);
		rename(LOG_PATH, LOG_PREVIOUS_PATH);
	}
	log_write("---- Imago started ----");
}

void log_write(const char *fmt, ...) {
	char line[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	for (char *p = line; *p; p++) // one entry per line
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	SceDateTime now; // the system clock; newlib's time() isn't reliable everywhere
	memset(&now, 0, sizeof(now));
	sceRtcGetCurrentClockLocalTime(&now);

	pthread_mutex_lock(&lock);
	FILE *f = fopen(LOG_PATH, "ab");
	if (f) {
		fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", now.year, now.month, now.day, now.hour, now.minute,
		        now.second, line);
		fclose(f);
	}
	pthread_mutex_unlock(&lock);
}
