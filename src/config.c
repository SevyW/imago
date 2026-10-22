#include "config.h"

#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH DATA_DIR "/config.txt"

Config g_config;

static const char *const devices[PARTITION_COUNT] = {"ux0:", "ur0:", "uma0:"};

const char *config_partition_device(int partition) {
	return partition >= 0 && partition < PARTITION_COUNT ? devices[partition] : devices[0];
}

static unsigned long read_uint(const char *text, const char *key) {
	char needle[48];
	snprintf(needle, sizeof(needle), "\n%s=", key);
	const char *p = strstr(text, needle);
	return p ? strtoul(p + strlen(needle), NULL, 10) : 0;
}

void config_load(void) {
	memset(&g_config, 0, sizeof(g_config));
	char *raw;
	if (fs_read_file(CONFIG_PATH, &raw, NULL) != 0)
		return;
	// Prefix a newline so every key, including the first, matches "\nkey=".
	char text[512];
	snprintf(text, sizeof(text), "\n%s", raw);
	free(raw);
	g_config.sort = (int)read_uint(text, "sort");
	g_config.partition = (int)read_uint(text, "partition");
	g_config.last_seen_id = (uint32_t)read_uint(text, "last_seen_id");
	g_config.share_photos = read_uint(text, "share_photos") != 0;
	if (g_config.sort < 0 || g_config.sort >= 6)
		g_config.sort = 0;
	if (g_config.partition < 0 || g_config.partition >= PARTITION_COUNT)
		g_config.partition = PARTITION_UX0;
}

void config_save(void) {
	char text[256];
	int n = snprintf(text, sizeof(text), "sort=%d\npartition=%d\nlast_seen_id=%u\nshare_photos=%d\n", g_config.sort,
	                 g_config.partition, (unsigned)g_config.last_seen_id, g_config.share_photos);
	fs_mkdir_p(DATA_DIR);
	fs_write_file(CONFIG_PATH, text, (size_t)n);
}
