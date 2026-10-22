// Per-app backups and customizations. This is the only code that writes to
// the shell's artwork cache (ur0:appmeta/<TITLEID>).
//
// For each app, DATA_DIR/apps/<TITLEID>/ holds:
//   original/      a copy of the app's appmeta folder, taken before the first change
//   original.txt   the CRC of every file in original/
//   custom/        the finished images Imago puts in place (icon.png, bg.png,
//                  gate.png)
//   custom.txt     every appmeta file Imago wrote, with its CRC
//   history/<n>/   older originals, kept when an app update brought new artwork
//
// Functions that can fail take an `err` buffer of STORE_ERROR_SIZE bytes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#ifndef APPMETA_DIR // overridden by host-side tests
#define APPMETA_DIR "ur0:appmeta"
#endif
#define STORE_DIR DATA_DIR "/apps"
#define STORE_ERROR_SIZE 160

typedef enum { ART_ICON, ART_BG, ART_GATE, ART_COUNT } ArtPart;

typedef enum {
	STORE_STOCK,   // nothing customized
	STORE_APPLIED, // customized, and every file Imago wrote is still in place
	STORE_DRIFTED, // customized, but some files were replaced (app update, database rebuild)
} StoreState;

// Title IDs are nine capital letters and digits, e.g. PCSB00245 or VITASHELL.
int store_valid_id(const char *id);
void store_appmeta_path(const char *id, const char *rel, char *out, size_t size);
// Where a part's finished file lives (custom/icon.png, bg.png or gate.png).
void store_custom_path(const char *id, ArtPart part, char *out, size_t size);
int store_has_custom(const char *id, ArtPart part);

// Copies the app's appmeta folder to original/ unless that was done before.
int store_backup(const char *id, char *err);
// Puts every customized part in place. Backs up first.
int store_apply(const char *id, char *err);
StoreState store_state(const char *id);
// For drifted apps: if the system brought new artwork (an app update), the
// old originals move to history/ and the new ones are backed up; then every
// customized part is put back.
int store_reapply(const char *id, char *err);
// Removes one customization and puts that part's original files back.
int store_clear_part(const char *id, ArtPart part, char *err);
// Removes every customization and puts all original files back.
int store_restore(const char *id, char *err);

uint32_t store_crc32_file(const char *path, int *ok);
