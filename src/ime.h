// System on-screen keyboard.
#pragma once

#include <stddef.h>

int ime_open(const char *title, const char *initial, int max_length);
// For keys and codes: Latin letters, no automatic capitals, no suggestions.
int ime_open_code(const char *title, const char *initial, int max_length);
// 1 = confirmed (out filled), -1 = canceled, 0 = still open.
int ime_poll(char *out, size_t size);
int ime_active(void);
