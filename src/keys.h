// API keys for the online artwork sources (ux0:data/Imago/keys.txt, one
// "name=value" per line). Never logged. Safe from any thread.
#pragma once

#include <stddef.h>

#include "config.h"

#define KEYS_PATH DATA_DIR "/keys.txt"
#define KEY_VALUE_MAX 256

void keys_load(void);
// The value, or "" when it isn't set.
void keys_get(const char *name, char *out, size_t size);
int keys_has(const char *name);
// An empty value removes the key. Saved straight away.
void keys_set(const char *name, const char *value);

// Takes "name=value" lines (from a QR code or a file); names not in `known`
// (NULL-terminated) are ignored. Returns how many keys were set.
int keys_import(const char *text, const char *const *known);
