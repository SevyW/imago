#include "at9.h"

#include <string.h>

// KSDATAFORMAT_SUBTYPE_ATRAC9: 47E142D2-36BA-4D8D-88FC-61654F8C836C
static const uint8_t atrac9_guid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                        0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p) {
	return (uint16_t)(p[0] | p[1] << 8);
}

int at9_parse(const uint8_t *buf, size_t size, At9Info *info) {
	memset(info, 0, sizeof(*info));
	if (size < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
		return -1;
	int have_fmt = 0;
	size_t pos = 12;
	while (pos + 8 <= size) {
		const uint8_t *chunk = buf + pos;
		uint32_t len = rd32(chunk + 4);
		size_t body = pos + 8;
		if (memcmp(chunk, "fmt ", 4) == 0) {
			// WAVEFORMATEXTENSIBLE: the ATRAC9 config follows the GUID.
			if (len < 52 || body + 52 > size)
				return -1;
			const uint8_t *f = buf + body;
			if (rd16(f) != 0xFFFE || memcmp(f + 24, atrac9_guid, 16) != 0)
				return -1;
			info->channels = rd16(f + 2);
			info->sample_rate = (int)rd32(f + 4);
			info->block_align = rd16(f + 12);
			memcpy(info->config, f + 44, 4);
			have_fmt = 1;
		} else if (memcmp(chunk, "data", 4) == 0) {
			if (!have_fmt || len == 0 || body + len > size)
				return -1;
			info->data_offset = (uint32_t)body;
			info->data_size = len;
			return info->channels >= 1 && info->channels <= 2 && info->sample_rate > 0 ? 0 : -1;
		}
		size_t next = body + len + (len & 1); // chunks are word-aligned
		if (next <= pos)
			return -1;
		pos = next;
	}
	return -1;
}
