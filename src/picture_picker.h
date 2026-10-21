// A full-screen grid of the pictures on the memory card, one folder at a time,
// for choosing wallpapers or a lock screen. Main thread only.
#pragma once

#include "input.h"

// paths are valid only during the call; count is 0 when cancelled.
typedef void (*PicturePickFn)(const char *const *paths, int count);

// With max_pick == 1, choosing a picture finishes straight away.
void picture_picker_open(const char *title, int max_pick, PicturePickFn done);
int picture_picker_is_open(void);
void picture_picker_update(const Input *in);
void picture_picker_draw(void);
