// Reading text columns of the system's app.db. The shell stores many strings
// (theme titles and authors, for one) as UTF-16LE blobs, even in rows that
// were written as UTF-8 text: reading those as text stops at the first byte.
#pragma once

#include <stddef.h>

#include "sqlite3.h"

// Copies a column as UTF-8 text: blobs are decoded as UTF-16LE, text is
// copied as it is. NULL becomes "".
void db_column_string(sqlite3_stmt *stmt, int col, char *out, size_t size);
