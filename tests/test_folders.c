// Host-side tests for Library folders. Build with -DFOLDERS_FILE='"<scratch>/folders.txt"'.
#include "../src/folders.h"
#include "../src/fs.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

int main(void) {
	fs_rm_rf(FOLDERS_FILE);
	fs_rm_rf("tf_themes");
	static const char *const themes[] = {"tf_themes/A", "tf_themes/B", "tf_themes/C", "tf_themes/D"};
	for (int i = 0; i < 4; i++)
		fs_mkdir_p(themes[i]); // themes that are gone are forgotten on load
	folders_load();
	CHECK(folders_count() == 0);
	CHECK(folders_of("tf_themes/A") == -1);

	int anime = folders_create("  Anime  ");
	int retro = folders_create("Retro");
	CHECK(anime == 0 && retro == 1);
	CHECK(strcmp(folders_name(anime), "Anime") == 0); // trimmed
	CHECK(folders_create("anime") == -1);             // names are unique, ignoring case
	CHECK(folders_create("   ") == -1);
	CHECK(folders_create("Tab\there") == 2);           // tabs can't break the file
	CHECK(strcmp(folders_name(2), "Tab here") == 0);

	folders_move("tf_themes/A", anime);
	folders_move("tf_themes/B", retro);
	folders_move("tf_themes/C", retro);
	folders_move("tf_themes/B", anime); // moving again just changes the folder
	CHECK(folders_of("tf_themes/A") == anime);
	CHECK(folders_of("tf_themes/B") == anime);
	CHECK(folders_of("tf_themes/C") == retro);
	folders_move("tf_themes/A", -1);
	CHECK(folders_of("tf_themes/A") == -1);

	CHECK(folders_rename(retro, "Classics") == 0);
	CHECK(folders_rename(retro, "ANIME") != 0); // taken by another folder
	CHECK(folders_rename(retro, "classics") == 0); // a new case for its own name is fine

	// Everything survives a reload.
	folders_load();
	CHECK(folders_count() == 3);
	CHECK(strcmp(folders_name(1), "classics") == 0);
	CHECK(folders_of("tf_themes/B") == 0);
	CHECK(folders_of("tf_themes/C") == 1);
	CHECK(folders_of("tf_themes/A") == -1);

	// Deleting a folder sends its themes back to the top and renumbers the rest.
	folders_move("tf_themes/D", 2);
	folders_delete(0);
	CHECK(folders_count() == 2);
	CHECK(folders_of("tf_themes/B") == -1);
	CHECK(folders_of("tf_themes/C") == 0);
	CHECK(folders_of("tf_themes/D") == 1);
	folders_load();
	CHECK(folders_of("tf_themes/D") == 1 && strcmp(folders_name(1), "Tab here") == 0);

	// An uninstalled theme drops out of its folder on the next load.
	fs_rm_rf("tf_themes/D");
	folders_load();
	CHECK(folders_of("tf_themes/D") == -1 && folders_count() == 2);

	// No more than FOLDERS_MAX folders.
	char name[16];
	int made = folders_count();
	for (int i = 0; i < FOLDERS_MAX + 5; i++) {
		snprintf(name, sizeof(name), "F%d", i);
		if (folders_create(name) >= 0)
			made++;
	}
	CHECK(made == FOLDERS_MAX && folders_count() == FOLDERS_MAX);

	// A damaged file loads what it can.
	const char *damaged = "folder\tOk\ngarbage\ntheme\tnofolder\ntheme\ttf_themes/gone\tMissing\ntheme\ttf_themes/A\tOk\n";
	fs_write_file(FOLDERS_FILE, damaged, strlen(damaged));
	folders_load();
	CHECK(folders_count() == 1 && folders_of("tf_themes/A") == 0 && folders_of("tf_themes/gone") == -1);

	printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
	return failures != 0;
}
