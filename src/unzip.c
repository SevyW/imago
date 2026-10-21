#include "unzip.h"

#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define IN_CHUNK (64 * 1024)
#define OUT_CHUNK (128 * 1024)

typedef struct {
	char name[256];
	uint16_t method;
	uint32_t crc;
	uint32_t csize;
	uint32_t usize;
	uint32_t local_offset;
} Entry;

static uint16_t rd16(const uint8_t *p) {
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *unzip_strerror(int code) {
	switch (code) {
	case UNZIP_OK: return "OK";
	case UNZIP_ERR_OPEN: return "Could not open the archive";
	case UNZIP_ERR_FORMAT: return "Not a valid zip archive";
	case UNZIP_ERR_UNSUPPORTED: return "Unsupported zip compression";
	case UNZIP_ERR_WRITE: return "Could not write to the memory card";
	case UNZIP_ERR_CORRUPT: return "The archive is corrupted";
	case UNZIP_ERR_CANCELED: return "Canceled";
	case UNZIP_ERR_NOMEM: return "Out of memory";
	default: return "Unknown zip error";
	}
}

// Returns 0 if the name is safe to extract, after normalising separators.
static int sanitize(char *name) {
	for (char *p = name; *p; p++)
		if (*p == '\\')
			*p = '/';
	if (name[0] == '/' || strchr(name, ':'))
		return -1;
	if (strncmp(name, "__MACOSX/", 9) == 0)
		return -1;
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	if (strcmp(base, ".DS_Store") == 0)
		return -1;
	for (const char *p = name; *p;) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 2 && p[0] == '.' && p[1] == '.')
			return -1;
		if (!slash)
			break;
		p = slash + 1;
	}
	return name[0] ? 0 : -1;
}

// Length of a top-level folder shared by every entry ("Folder/"), or 0.
static size_t common_prefix(const Entry *entries, int count) {
	if (count == 0)
		return 0;
	const char *slash = strchr(entries[0].name, '/');
	if (!slash)
		return 0;
	size_t prefix = (size_t)(slash - entries[0].name) + 1;
	for (int i = 1; i < count; i++)
		if (strncmp(entries[i].name, entries[0].name, prefix) != 0)
			return 0;
	return prefix;
}

typedef struct {
	FILE *zip;
	unsigned char *in;
	unsigned char *out;
	uint64_t done, total;
	UnzipProgress progress;
	void *user;
} Ctx;

static int report(Ctx *c, size_t n) {
	c->done += n;
	if (c->progress && c->progress(c->user, c->done, c->total))
		return UNZIP_ERR_CANCELED;
	return UNZIP_OK;
}

static int extract_entry(Ctx *c, const Entry *e, const char *path) {
	uint8_t hdr[30];
	if (fseek(c->zip, (long)e->local_offset, SEEK_SET) != 0 || fread(hdr, 1, 30, c->zip) != 30 ||
	    rd32(hdr) != 0x04034b50)
		return UNZIP_ERR_CORRUPT;
	long data = (long)e->local_offset + 30 + rd16(hdr + 26) + rd16(hdr + 28);
	if (fseek(c->zip, data, SEEK_SET) != 0)
		return UNZIP_ERR_CORRUPT;

	FILE *out = fopen(path, "wb");
	if (!out)
		return UNZIP_ERR_WRITE;

	int rc = UNZIP_OK;
	uLong crc = crc32(0, Z_NULL, 0);
	uint32_t remaining = e->csize, produced = 0;

	if (e->method == 0) {
		while (rc == UNZIP_OK && remaining > 0) {
			size_t n = remaining < OUT_CHUNK ? remaining : OUT_CHUNK;
			if (fread(c->out, 1, n, c->zip) != n) {
				rc = UNZIP_ERR_CORRUPT;
				break;
			}
			if (fwrite(c->out, 1, n, out) != n) {
				rc = UNZIP_ERR_WRITE;
				break;
			}
			crc = crc32(crc, c->out, (uInt)n);
			remaining -= (uint32_t)n;
			produced += (uint32_t)n;
			rc = report(c, n);
		}
	} else if (e->method == 8) {
		z_stream zs;
		memset(&zs, 0, sizeof(zs));
		if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
			fclose(out);
			return UNZIP_ERR_NOMEM;
		}
		int z = Z_OK;
		while (rc == UNZIP_OK && z != Z_STREAM_END) {
			if (zs.avail_in == 0) {
				if (remaining == 0) {
					rc = UNZIP_ERR_CORRUPT;
					break;
				}
				size_t n = remaining < IN_CHUNK ? remaining : IN_CHUNK;
				if (fread(c->in, 1, n, c->zip) != n) {
					rc = UNZIP_ERR_CORRUPT;
					break;
				}
				remaining -= (uint32_t)n;
				zs.next_in = c->in;
				zs.avail_in = (uInt)n;
			}
			zs.next_out = c->out;
			zs.avail_out = OUT_CHUNK;
			z = inflate(&zs, Z_NO_FLUSH);
			if (z != Z_OK && z != Z_STREAM_END) {
				rc = UNZIP_ERR_CORRUPT;
				break;
			}
			size_t n = OUT_CHUNK - zs.avail_out;
			if ((uint64_t)produced + n > e->usize) {
				rc = UNZIP_ERR_CORRUPT; // inflates past its declared size
				break;
			}
			if (n) {
				if (fwrite(c->out, 1, n, out) != n) {
					rc = UNZIP_ERR_WRITE;
					break;
				}
				crc = crc32(crc, c->out, (uInt)n);
				produced += (uint32_t)n;
				rc = report(c, n);
			}
		}
		inflateEnd(&zs);
	} else {
		rc = UNZIP_ERR_UNSUPPORTED;
	}

	if (fclose(out) != 0 && rc == UNZIP_OK)
		rc = UNZIP_ERR_WRITE;
	if (rc == UNZIP_OK && (crc != e->crc || produced != e->usize))
		rc = UNZIP_ERR_CORRUPT;
	if (rc != UNZIP_OK)
		remove(path);
	return rc;
}

static int read_directory(FILE *f, Entry **out_entries, int *out_count) {
	if (fseek(f, 0, SEEK_END) != 0)
		return UNZIP_ERR_FORMAT;
	long size = ftell(f);
	if (size < 22)
		return UNZIP_ERR_FORMAT;
	long tail = size < 65557 ? size : 65557;
	uint8_t *buf = malloc((size_t)tail);
	if (!buf)
		return UNZIP_ERR_NOMEM;
	if (fseek(f, size - tail, SEEK_SET) != 0 || fread(buf, 1, (size_t)tail, f) != (size_t)tail) {
		free(buf);
		return UNZIP_ERR_FORMAT;
	}
	const uint8_t *eocd = NULL;
	for (long i = tail - 22; i >= 0; i--) {
		if (rd32(buf + i) == 0x06054b50) {
			eocd = buf + i;
			break;
		}
	}
	if (!eocd) {
		free(buf);
		return UNZIP_ERR_FORMAT;
	}
	int total = rd16(eocd + 10);
	uint32_t cd_size = rd32(eocd + 12);
	uint32_t cd_offset = rd32(eocd + 16);
	free(buf);
	if (cd_offset == 0xFFFFFFFFu || (uint64_t)cd_offset + cd_size > (uint64_t)size)
		return UNZIP_ERR_UNSUPPORTED; // zip64 is never used for themes

	uint8_t *cd = malloc(cd_size ? cd_size : 1);
	Entry *entries = calloc(total ? (size_t)total : 1, sizeof(Entry));
	if (!cd || !entries) {
		free(cd);
		free(entries);
		return UNZIP_ERR_NOMEM;
	}
	if (fseek(f, (long)cd_offset, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) {
		free(cd);
		free(entries);
		return UNZIP_ERR_FORMAT;
	}

	int count = 0;
	const uint8_t *p = cd, *end = cd + cd_size;
	for (int i = 0; i < total && p + 46 <= end && rd32(p) == 0x02014b50; i++) {
		uint16_t flags = rd16(p + 8);
		uint16_t nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
		if (p + 46 + nlen > end)
			break;
		Entry *e = &entries[count];
		size_t n = nlen < sizeof(e->name) - 1 ? nlen : sizeof(e->name) - 1;
		memcpy(e->name, p + 46, n);
		e->name[n] = '\0';
		e->method = rd16(p + 10);
		e->crc = rd32(p + 16);
		e->csize = rd32(p + 20);
		e->usize = rd32(p + 24);
		e->local_offset = rd32(p + 42);
		if (flags & 1) { // encrypted
			free(cd);
			free(entries);
			return UNZIP_ERR_UNSUPPORTED;
		}
		if (n == nlen && sanitize(e->name) == 0)
			count++;
		p += 46 + nlen + xlen + clen;
	}
	free(cd);
	*out_entries = entries;
	*out_count = count;
	return UNZIP_OK;
}

int unzip_measure(const char *zip_path, uint64_t *total) {
	FILE *f = fopen(zip_path, "rb");
	if (!f)
		return UNZIP_ERR_OPEN;
	Entry *entries = NULL;
	int count = 0;
	int rc = read_directory(f, &entries, &count);
	fclose(f);
	*total = 0;
	for (int i = 0; rc == UNZIP_OK && i < count; i++)
		*total += entries[i].usize;
	free(entries);
	return rc;
}

int unzip_extract(const char *zip_path, const char *dest_dir, UnzipProgress progress, void *user,
                  uint64_t *bytes_written) {
	FILE *f = fopen(zip_path, "rb");
	if (!f)
		return UNZIP_ERR_OPEN;

	Entry *entries = NULL;
	int count = 0;
	int rc = read_directory(f, &entries, &count);
	if (rc != UNZIP_OK) {
		fclose(f);
		return rc;
	}

	// Strip a shared top-level folder unless theme.xml already sits at the root.
	size_t strip = 0;
	int root_xml = 0;
	for (int i = 0; i < count; i++)
		if (strcmp(entries[i].name, "theme.xml") == 0)
			root_xml = 1;
	if (!root_xml)
		strip = common_prefix(entries, count);

	Ctx c = {f, malloc(IN_CHUNK), malloc(OUT_CHUNK), 0, 0, progress, user};
	if (!c.in || !c.out)
		rc = UNZIP_ERR_NOMEM;
	for (int i = 0; i < count; i++)
		c.total += entries[i].usize;

	if (rc == UNZIP_OK && fs_mkdir_p(dest_dir) != 0)
		rc = UNZIP_ERR_WRITE;

	for (int i = 0; rc == UNZIP_OK && i < count; i++) {
		const char *rel = entries[i].name;
		if (strip && strlen(rel) >= strip)
			rel += strip;
		if (!rel[0])
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dest_dir, rel);
		size_t len = strlen(path);
		if (path[len - 1] == '/') {
			if (fs_mkdir_p(path) != 0)
				rc = UNZIP_ERR_WRITE;
			continue;
		}
		char *slash = strrchr(path, '/');
		if (slash) {
			*slash = '\0';
			if (fs_mkdir_p(path) != 0)
				rc = UNZIP_ERR_WRITE;
			*slash = '/';
		}
		if (rc == UNZIP_OK)
			rc = extract_entry(&c, &entries[i], path);
	}

	if (bytes_written)
		*bytes_written = c.done;
	free(c.in);
	free(c.out);
	free(entries);
	fclose(f);
	return rc;
}
