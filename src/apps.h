// The installed apps whose artwork Imago can change: every folder in
// ur0:appmeta, with its title and customization state.
#pragma once

#include "store.h"

typedef struct {
	char id[16];
	char title[128];
	int system;       // a built-in app (NPXS...)
	int has_livearea; // has a LiveArea template Imago understands
	StoreState state;
} AppItem;

typedef struct {
	AppItem *items;
	int count;
} AppList;

// Builds the list, sorted by title. Slow (reads the memory card): run it on a worker.
int apps_scan(AppList *out);
void apps_free(AppList *list);
const AppItem *apps_find(const AppList *list, const char *id);
