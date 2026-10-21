// Modal dialogs drawn above every screen. Only one is open at a time.
#pragma once

#include "input.h"

// choice: the selected option index, or -1 when dismissed.
typedef void (*DialogFn)(int choice);

// Options prefixed with '!' are drawn as destructive.
void dialog_menu(const char *title, const char *const *options, int count, int selected, DialogFn fn);
// Two buttons: cancel (-1) and confirm (0). Destructive dialogs focus Cancel first.
void dialog_confirm(const char *title, const char *message, const char *confirm_label, int destructive,
                    DialogFn fn);
void dialog_message(const char *title, const char *message);

int dialog_open(void);
void dialog_update(const Input *in);
void dialog_draw(void);
