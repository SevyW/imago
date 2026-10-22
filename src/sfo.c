#include "sfo.h"

#include "fs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SFO_FMT_UTF8 0x0204
#define SFO_FMT_UTF8_SPECIAL 0x0004

static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p) {
	return (uint16_t)(p[0] | p[1] << 8);
}

int sfo_get_string(const unsigned char *data, size_t size, const char *key, char *out, size_t out_size) {
	if (out_size)
		out[0] = '\0';
	if (size < 20 || memcmp(data, "\0PSF", 4) != 0)
		return -1;
	uint32_t keys = le32(data + 8), values = le32(data + 12), count = le32(data + 16);
	if (keys > size || values > size || count > (size - 20) / 16)
		return -1;
	for (uint32_t i = 0; i < count; i++) {
		const unsigned char *e = data + 20 + i * 16;
		uint32_t key_at = keys + le16(e);
		uint16_t fmt = le16(e + 2);
		uint32_t len = le32(e + 4), value_at = values + le32(e + 12);
		if (key_at >= size || memchr(data + key_at, '\0', size - key_at) == NULL)
			return -1;
		if (strcmp((const char *)data + key_at, key) != 0)
			continue;
		if ((fmt != SFO_FMT_UTF8 && fmt != SFO_FMT_UTF8_SPECIAL) || value_at > size || len > size - value_at)
			return -1;
		size_t n = 0;
		while (n < len && data[value_at + n] != '\0')
			n++;
		if (n >= out_size)
			n = out_size - 1;
		memcpy(out, data + value_at, n);
		out[n] = '\0';
		return 0;
	}
	return -1;
}

int sfo_read_string(const char *path, const char *key, char *out, size_t out_size) {
	char *data;
	size_t size;
	if (out_size)
		out[0] = '\0';
	if (fs_read_file(path, &data, &size) != 0)
		return -1;
	int rc = sfo_get_string((const unsigned char *)data, size, key, out, out_size);
	free(data);
	return rc;
}
