// Persistent user settings (ux0:data/Imago/config.txt).
#pragma once

#include <stdint.h>

#ifndef DATA_DIR // overridden by host-side tests
#define DATA_DIR "ux0:data/Imago"
#endif

// Theme database backups. The manual one is only written from Settings; the
// automatic ones rotate (auto-1 is the newest) so a bad session can never
// overwrite every good copy.
#define BACKUP_DIR DATA_DIR "/backups"
#define BACKUP_MANUAL BACKUP_DIR "/manual.db"
#define BACKUP_AUTO_FORMAT BACKUP_DIR "/auto-%d.db"
#define BACKUP_AUTO_COUNT 3

enum { PARTITION_UX0, PARTITION_UR0, PARTITION_UMA0, PARTITION_COUNT };

typedef struct {
	int sort;              // PsvtSort
	int partition;         // where new themes are installed
	uint32_t last_seen_id; // newest theme id seen last session ("NEW" badges)
	int share_photos;      // add theme wallpapers to the Photos app (photos.h)
	int show_system;       // Apps tab: list system apps (NPXS...) too
} Config;

extern Config g_config;

void config_load(void);
void config_save(void);

const char *config_partition_device(int partition); // "ux0:"
