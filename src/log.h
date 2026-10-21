// Appends diagnostic lines to ux0:data/Imago/log.txt (thread-safe).
#pragma once

void log_init(void);
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
