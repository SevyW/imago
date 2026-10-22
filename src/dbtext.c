#include "dbtext.h"

#include <stdint.h>
#include <stdio.h>

static size_t put_utf8(char *out, size_t size, size_t n, uint32_t c) {
	char buf[4];
	size_t len;
	if (c < 0x80) {
		buf[0] = (char)c;
		len = 1;
	} else if (c < 0x800) {
		buf[0] = (char)(0xC0 | c >> 6);
		buf[1] = (char)(0x80 | (c & 0x3F));
		len = 2;
	} else if (c < 0x10000) {
		buf[0] = (char)(0xE0 | c >> 12);
		buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (c & 0x3F));
		len = 3;
	} else {
		buf[0] = (char)(0xF0 | c >> 18);
		buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
		buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
		buf[3] = (char)(0x80 | (c & 0x3F));
		len = 4;
	}
	if (n + len >= size)
		return n; // no room: stop at a whole character
	for (size_t i = 0; i < len; i++)
		out[n + i] = buf[i];
	return n + len;
}

void db_column_string(sqlite3_stmt *stmt, int col, char *out, size_t size) {
	if (size == 0)
		return;
	out[0] = '\0';
	if (sqlite3_column_type(stmt, col) != SQLITE_BLOB) {
		const unsigned char *text = sqlite3_column_text(stmt, col);
		snprintf(out, size, "%s", text ? (const char *)text : "");
		return;
	}
	const unsigned char *b = sqlite3_column_blob(stmt, col);
	int bytes = sqlite3_column_bytes(stmt, col);
	size_t n = 0;
	for (int i = 0; b && i + 1 < bytes;) {
		uint32_t c = (uint32_t)(b[i] | b[i + 1] << 8);
		i += 2;
		if (c == 0)
			break;
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < bytes) { // a surrogate pair
			uint32_t low = (uint32_t)(b[i] | b[i + 1] << 8);
			if (low >= 0xDC00 && low < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
				i += 2;
			}
		}
		size_t before = n;
		n = put_utf8(out, size, n, c);
		if (n == before)
			break;
	}
	out[n] = '\0';
}
