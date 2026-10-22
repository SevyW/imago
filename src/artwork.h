// Online artwork: searching artwork sites for an app's bubble icon, LiveArea
// background or start gate. search and test block on the network: call them
// from a job (worker thread) only.
#pragma once

#include <stddef.h>

#include "net.h"
#include "store.h" // ArtPart

#define ART_MAX_RESULTS 60
#define ART_ERROR_SIZE 200

typedef struct {
	char thumb[512]; // a small preview
	char full[512];  // the picture itself (PNG or JPEG)
	char label[96];  // what it is: the game's name, the kind of picture
	int w, h;        // 0 when unknown
} ArtResult;

typedef struct {
	ArtResult items[ART_MAX_RESULTS];
	int count;
} ArtResults;

typedef struct {
	char title[128];   // what to search for (the app's name, or what the user typed)
	char title_id[16]; // PCSE00120, VITASHELL...
	ArtPart part;
} ArtQuery;

typedef struct {
	const char *id;   // also the prefix of its keys: "steamgriddb"
	const char *name; // "SteamGridDB"
	// The keys it needs (NULL-terminated, NULL when it needs none), and how
	// to ask for each one.
	const char *const *keys;
	const char *const *key_prompts;
	const char *help;   // where to get a key, shown in Settings
	const char *credit; // shown under its results ("rawg.io"); NULL for none
	unsigned parts;     // which parts it has pictures for: 1 << ArtPart
	int (*search)(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t err_size);
	// A cheap call that proves the keys work. 0 when they do.
	int (*test)(NetClient *net, char *err, size_t err_size);
} ArtSource;

int artwork_count(void);
const ArtSource *artwork_get(int index);
const ArtSource *artwork_find(const char *id);
// Every key the source needs is set (always true for keyless ones).
int artwork_ready(const ArtSource *s);
// Every key name of every source, NULL-terminated (for keys_import).
const char *const *artwork_key_names(void);
