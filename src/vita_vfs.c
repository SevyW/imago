// SQLite OS layer for the Vita (sqlite3.c is built with SQLITE_OS_OTHER=1).
// Plain sceIo calls with no file locking. The shell also writes app.db, so
// themedb.c refuses to write while the shell's rollback journal is live, keeps
// every write to one short transaction, and backs the database up first.
#include "sqlite3.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEMP_DIR "ux0:data/Imago"

typedef struct {
	sqlite3_file base;
	SceUID fd;
	int delete_on_close;
	char path[256];
} VitaFile;

static int vf_close(sqlite3_file *file) {
	VitaFile *f = (VitaFile *)file;
	sceIoClose(f->fd);
	if (f->delete_on_close)
		sceIoRemove(f->path);
	return SQLITE_OK;
}

static int vf_read(sqlite3_file *file, void *buf, int amount, sqlite3_int64 offset) {
	VitaFile *f = (VitaFile *)file;
	int n = sceIoPread(f->fd, buf, (SceSize)amount, offset);
	if (n < 0)
		return SQLITE_IOERR_READ;
	if (n < amount) {
		memset((char *)buf + n, 0, (size_t)(amount - n));
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

static int vf_write(sqlite3_file *file, const void *buf, int amount, sqlite3_int64 offset) {
	VitaFile *f = (VitaFile *)file;
	const char *p = buf;
	while (amount > 0) {
		int n = sceIoPwrite(f->fd, p, (SceSize)amount, offset);
		if (n <= 0)
			return SQLITE_IOERR_WRITE;
		p += n;
		offset += n;
		amount -= n;
	}
	return SQLITE_OK;
}

static int vf_truncate(sqlite3_file *file, sqlite3_int64 size) {
	VitaFile *f = (VitaFile *)file;
	SceIoStat st;
	memset(&st, 0, sizeof(st));
	st.st_size = size;
	return sceIoChstatByFd(f->fd, &st, SCE_CST_SIZE) < 0 ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int vf_sync(sqlite3_file *file, int flags) {
	(void)flags;
	VitaFile *f = (VitaFile *)file;
	return sceIoSyncByFd(f->fd, 0) < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int vf_file_size(sqlite3_file *file, sqlite3_int64 *size) {
	VitaFile *f = (VitaFile *)file;
	SceIoStat st;
	if (sceIoGetstatByFd(f->fd, &st) < 0)
		return SQLITE_IOERR_FSTAT;
	*size = st.st_size;
	return SQLITE_OK;
}

static int vf_lock(sqlite3_file *file, int level) {
	(void)file;
	(void)level;
	return SQLITE_OK;
}

static int vf_check_reserved_lock(sqlite3_file *file, int *out) {
	(void)file;
	*out = 0;
	return SQLITE_OK;
}

static int vf_file_control(sqlite3_file *file, int op, void *arg) {
	(void)file;
	(void)op;
	(void)arg;
	return SQLITE_NOTFOUND;
}

static int vf_sector_size(sqlite3_file *file) {
	(void)file;
	return 512;
}

static int vf_device_characteristics(sqlite3_file *file) {
	(void)file;
	return 0;
}

static const sqlite3_io_methods io_methods = {
	.iVersion = 1,
	.xClose = vf_close,
	.xRead = vf_read,
	.xWrite = vf_write,
	.xTruncate = vf_truncate,
	.xSync = vf_sync,
	.xFileSize = vf_file_size,
	.xLock = vf_lock,
	.xUnlock = vf_lock,
	.xCheckReservedLock = vf_check_reserved_lock,
	.xFileControl = vf_file_control,
	.xSectorSize = vf_sector_size,
	.xDeviceCharacteristics = vf_device_characteristics,
};

static int vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out_flags) {
	(void)vfs;
	static int temp_counter;
	VitaFile *f = (VitaFile *)file;
	memset(f, 0, sizeof(*f));
	if (name)
		snprintf(f->path, sizeof(f->path), "%s", name);
	else
		snprintf(f->path, sizeof(f->path), TEMP_DIR "/sqlite_%d.tmp", ++temp_counter);

	int mode = (flags & SQLITE_OPEN_READWRITE) ? SCE_O_RDWR : SCE_O_RDONLY;
	if (flags & SQLITE_OPEN_CREATE)
		mode |= SCE_O_CREAT;
	if (flags & SQLITE_OPEN_EXCLUSIVE)
		mode |= SCE_O_EXCL;

	f->fd = sceIoOpen(f->path, mode, 0666);
	if (f->fd < 0 && (flags & SQLITE_OPEN_READWRITE) && !(flags & SQLITE_OPEN_CREATE)) {
		f->fd = sceIoOpen(f->path, SCE_O_RDONLY, 0);
		if (f->fd >= 0)
			flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
	}
	if (f->fd < 0)
		return SQLITE_CANTOPEN;

	f->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) || !name;
	f->base.pMethods = &io_methods;
	if (out_flags)
		*out_flags = flags;
	return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
	(void)vfs;
	(void)sync_dir;
	SceIoStat st;
	if (sceIoGetstat(name, &st) < 0)
		return SQLITE_OK;
	return sceIoRemove(name) < 0 ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
	(void)vfs;
	(void)flags;
	SceIoStat st;
	*out = sceIoGetstat(name, &st) >= 0;
	return SQLITE_OK;
}

static int vfs_full_pathname(sqlite3_vfs *vfs, const char *name, int size, char *out) {
	(void)vfs;
	snprintf(out, (size_t)size, "%s", name);
	return SQLITE_OK;
}

static int vfs_randomness(sqlite3_vfs *vfs, int size, char *out) {
	(void)vfs;
	unsigned int seed = (unsigned int)sceKernelGetProcessTimeWide();
	for (int i = 0; i < size; i++) {
		seed = seed * 1103515245u + 12345u;
		out[i] = (char)(seed >> 16);
	}
	return size;
}

static int vfs_sleep(sqlite3_vfs *vfs, int microseconds) {
	(void)vfs;
	sceKernelDelayThread((SceUInt)microseconds);
	return microseconds;
}

static int vfs_current_time(sqlite3_vfs *vfs, double *out) {
	(void)vfs;
	*out = 2440587.5 + (double)time(NULL) / 86400.0;
	return SQLITE_OK;
}

static int vfs_get_last_error(sqlite3_vfs *vfs, int size, char *out) {
	(void)vfs;
	(void)size;
	(void)out;
	return 0;
}

static sqlite3_vfs vita_vfs = {
	.iVersion = 1,
	.szOsFile = sizeof(VitaFile),
	.mxPathname = 256,
	.zName = "psp2",
	.xOpen = vfs_open,
	.xDelete = vfs_delete,
	.xAccess = vfs_access,
	.xFullPathname = vfs_full_pathname,
	.xRandomness = vfs_randomness,
	.xSleep = vfs_sleep,
	.xCurrentTime = vfs_current_time,
	.xGetLastError = vfs_get_last_error,
};

int sqlite3_os_init(void) {
	return sqlite3_vfs_register(&vita_vfs, 1);
}

int sqlite3_os_end(void) {
	return SQLITE_OK;
}
