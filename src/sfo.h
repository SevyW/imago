// Reading strings from a param.sfo file.
#pragma once

#include <stddef.h>

// Copies the string value of `key` (e.g. "TITLE", "APP_VER") from the SFO
// data into out. Returns 0 when found.
int sfo_get_string(const unsigned char *data, size_t size, const char *key, char *out, size_t out_size);
// The same, reading the file first.
int sfo_read_string(const char *path, const char *key, char *out, size_t out_size);
