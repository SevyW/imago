// Minimal ZIP extractor (stored + deflate) on top of zlib.
#pragma once

#include <stdint.h>

enum {
	UNZIP_OK = 0,
	UNZIP_ERR_OPEN = -1,
	UNZIP_ERR_FORMAT = -2,
	UNZIP_ERR_UNSUPPORTED = -3,
	UNZIP_ERR_WRITE = -4,
	UNZIP_ERR_CORRUPT = -5,
	UNZIP_ERR_CANCELED = -6,
	UNZIP_ERR_NOMEM = -7,
};

// Called after every chunk; return nonzero to cancel.
typedef int (*UnzipProgress)(void *user, uint64_t done, uint64_t total);

// Extracts every entry into dest_dir. When all entries share a single top-level
// folder (a theme zipped together with its directory), that folder is stripped
// so theme.xml lands directly in dest_dir. Paths are sanitised: absolute paths,
// "..", and macOS metadata are skipped.
int unzip_extract(const char *zip_path, const char *dest_dir, UnzipProgress progress, void *user,
                  uint64_t *bytes_written);

// Sum of the uncompressed sizes declared by the archive, for space checks.
int unzip_measure(const char *zip_path, uint64_t *total);

const char *unzip_strerror(int code);
