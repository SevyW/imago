// Small filesystem helpers. Built on POSIX/stdio (which newlib maps onto sceIo
// on the Vita) so the same code runs in host-side tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

int fs_exists(const char *path);
int fs_is_dir(const char *path);
// Creates every missing directory in path. Returns 0 on success.
int fs_mkdir_p(const char *path);
// Deletes a file or a directory tree. Returns 0 on success (or if missing).
int fs_rm_rf(const char *path);
int64_t fs_tree_size(const char *path);
// Reads a whole file into a malloc'd, NUL-terminated buffer.
int fs_read_file(const char *path, char **data, size_t *size);
int fs_write_file(const char *path, const void *data, size_t size);
int fs_copy_file(const char *src, const char *dst);
int fs_rename(const char *from, const char *to);
// Free bytes on a device such as "ux0:", or -1 if unknown.
int64_t fs_free_space(const char *device);

// Calls cb for every entry except "." and "..". Returning nonzero stops.
typedef int (*FsDirCallback)(const char *name, int is_dir, void *user);
int fs_list_dir(const char *path, FsDirCallback cb, void *user);

// "12.3 MB" style formatting.
void fs_format_size(char *out, size_t size, int64_t bytes);
