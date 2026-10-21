// Your own folders for organising the Library. They're Imago's alone: the
// system theme list never sees them. Saved in ux0:data/Imago/folders.txt.
#pragma once

#define FOLDERS_MAX 32
#define FOLDER_NAME_MAX 48

void folders_load(void);
int folders_count(void);
const char *folders_name(int folder);
// Returns the new folder's index, or -1 if the name is empty, taken, or
// there are already FOLDERS_MAX folders.
int folders_create(const char *name);
int folders_rename(int folder, const char *name);
// Its themes go back to the top level of the Library.
void folders_delete(int folder);
// The folder a theme (by its directory) is in, or -1.
int folders_of(const char *theme_dir);
// folder -1 moves the theme back to the top level. Returns 0 on success.
int folders_move(const char *theme_dir, int folder);
