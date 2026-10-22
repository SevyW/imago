// Appends diagnostic lines to ux0:data/Imago/log.txt (thread-safe). Every
// line is written and closed straight away, so a crash loses nothing.
#pragma once

#include "config.h"

#define LOG_PATH DATA_DIR "/log.txt"
// The log before the last rotation, so a long session never pushes out
// what happened just before a crash.
#define LOG_PREVIOUS_PATH DATA_DIR "/log-previous.txt"

void log_init(void);
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
