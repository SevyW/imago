#include "folders.h"

#include "config.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef FOLDERS_FILE
#define FOLDERS_FILE DATA_DIR "/folders.txt"
#endif
#define MAX_PLACED 1024

// The file is plain text, one entry per line:
//   folder<TAB>Anime
//   theme<TAB>ux0:customtheme/Persona_5<TAB>Anime
// Themes refer to folders by name so the order of folders can never mix them up.

static char names[FOLDERS_MAX][FOLDER_NAME_MAX];
static int count;
static struct {
	char dir[256];
	int folder;
} placed[MAX_PLACED];
static int placed_count;

static void save(void) {
	size_t cap = 64 + (size_t)count * (FOLDER_NAME_MAX + 8) + (size_t)placed_count * (256 + FOLDER_NAME_MAX + 8);
	char *text = malloc(cap);
	if (!text)
		return;
	size_t len = 0;
	for (int i = 0; i < count; i++)
		len += (size_t)snprintf(text + len, cap - len, "folder\t%s\n", names[i]);
	for (int i = 0; i < placed_count; i++)
		len += (size_t)snprintf(text + len, cap - len, "theme\t%s\t%s\n", placed[i].dir, names[placed[i].folder]);
	fs_write_file(FOLDERS_FILE, text, len); // DATA_DIR is created at startup
	free(text);
}

static int find(const char *name) {
	for (int i = 0; i < count; i++)
		if (strcasecmp(names[i], name) == 0)
			return i;
	return -1;
}

// Tabs and newlines would break the file; names are trimmed and kept short.
static int clean_name(const char *name, char *out) {
	while (*name == ' ')
		name++;
	size_t n = 0;
	for (; *name && n + 1 < FOLDER_NAME_MAX; name++)
		out[n++] = (*name == '\t' || *name == '\n' || *name == '\r') ? ' ' : *name;
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
	return n > 0 ? 0 : -1;
}

void folders_load(void) {
	count = placed_count = 0;
	char *text;
	if (fs_read_file(FOLDERS_FILE, &text, NULL) != 0)
		return;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char *cr = strchr(line, '\r');
		if (cr)
			*cr = '\0';
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab++ = '\0';
		if (strcmp(line, "folder") == 0) {
			char name[FOLDER_NAME_MAX];
			if (count < FOLDERS_MAX && clean_name(tab, name) == 0 && find(name) < 0)
				snprintf(names[count++], FOLDER_NAME_MAX, "%s", name);
		} else if (strcmp(line, "theme") == 0) {
			char *folder = strchr(tab, '\t');
			if (!folder || placed_count == MAX_PLACED)
				continue;
			*folder++ = '\0';
			int f = find(folder);
			if (f >= 0 && tab[0]) {
				snprintf(placed[placed_count].dir, sizeof(placed[0].dir), "%s", tab);
				placed[placed_count++].folder = f;
			}
		}
	}
	free(text);

	// Uninstalled themes leave their entries behind; forget them.
	int kept = 0;
	for (int i = 0; i < placed_count; i++)
		if (fs_exists(placed[i].dir))
			placed[kept++] = placed[i];
	if (kept != placed_count) {
		placed_count = kept;
		save();
	}
}

int folders_count(void) {
	return count;
}

const char *folders_name(int folder) {
	return folder >= 0 && folder < count ? names[folder] : "";
}

int folders_create(const char *name) {
	char clean[FOLDER_NAME_MAX];
	if (count == FOLDERS_MAX || clean_name(name, clean) != 0 || find(clean) >= 0)
		return -1;
	snprintf(names[count], FOLDER_NAME_MAX, "%s", clean);
	count++;
	save();
	return count - 1;
}

int folders_rename(int folder, const char *name) {
	char clean[FOLDER_NAME_MAX];
	if (folder < 0 || folder >= count || clean_name(name, clean) != 0)
		return -1;
	int other = find(clean);
	if (other >= 0 && other != folder)
		return -1;
	snprintf(names[folder], FOLDER_NAME_MAX, "%s", clean);
	save();
	return 0;
}

void folders_delete(int folder) {
	if (folder < 0 || folder >= count)
		return;
	int kept = 0;
	for (int i = 0; i < placed_count; i++) {
		if (placed[i].folder == folder)
			continue;
		placed[kept] = placed[i];
		if (placed[kept].folder > folder)
			placed[kept].folder--;
		kept++;
	}
	placed_count = kept;
	memmove(names[folder], names[folder + 1], (size_t)(count - folder - 1) * FOLDER_NAME_MAX);
	count--;
	save();
}

int folders_of(const char *theme_dir) {
	for (int i = 0; i < placed_count; i++)
		if (strcmp(placed[i].dir, theme_dir) == 0)
			return placed[i].folder;
	return -1;
}

int folders_move(const char *theme_dir, int folder) {
	int at = -1;
	for (int i = 0; i < placed_count; i++)
		if (strcmp(placed[i].dir, theme_dir) == 0)
			at = i;
	if (folder < 0 || folder >= count) {
		if (at < 0)
			return 0;
		placed[at] = placed[--placed_count];
	} else if (at >= 0) {
		placed[at].folder = folder;
	} else if (placed_count < MAX_PLACED) {
		snprintf(placed[placed_count].dir, sizeof(placed[0].dir), "%s", theme_dir);
		placed[placed_count++].folder = folder;
	} else {
		return -1;
	}
	save();
	return 0;
}
