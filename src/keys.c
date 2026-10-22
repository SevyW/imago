#include "keys.h"

#include "fs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 24

typedef struct {
	char name[32];
	char value[KEY_VALUE_MAX];
} Key;

static Key keys[MAX_KEYS];
static int key_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Keys are pasted, typed and scanned: drop stray spaces and line breaks.
static void trim(char *s) {
	char *start = s;
	while (isspace((unsigned char)*start))
		start++;
	size_t n = strlen(start);
	while (n && isspace((unsigned char)start[n - 1]))
		n--;
	memmove(s, start, n);
	s[n] = '\0';
}

static Key *find(const char *name) {
	for (int i = 0; i < key_count; i++)
		if (strcmp(keys[i].name, name) == 0)
			return &keys[i];
	return NULL;
}

static void put(const char *name, const char *value) {
	Key *k = find(name);
	if (!value[0]) {
		if (k)
			*k = keys[--key_count];
		return;
	}
	if (!k && key_count < MAX_KEYS)
		k = &keys[key_count++];
	if (k) {
		snprintf(k->name, sizeof(k->name), "%s", name);
		snprintf(k->value, sizeof(k->value), "%s", value);
	}
}

static void save(void) {
	char text[MAX_KEYS * (32 + KEY_VALUE_MAX + 2) + 1];
	size_t len = 0;
	for (int i = 0; i < key_count; i++)
		len += (size_t)snprintf(text + len, sizeof(text) - len, "%s=%s\n", keys[i].name, keys[i].value);
	if (key_count)
		fs_write_file(KEYS_PATH, text, len);
	else
		remove(KEYS_PATH);
}

// Calls fn for each "name=value" line.
static int each_line(char *text, void (*fn)(const char *name, const char *value, void *user), void *user) {
	int n = 0;
	char *rest;
	for (char *line = strtok_r(text, "\r\n", &rest); line; line = strtok_r(NULL, "\r\n", &rest)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char name[32], value[KEY_VALUE_MAX];
		snprintf(name, sizeof(name), "%s", line);
		snprintf(value, sizeof(value), "%s", eq + 1);
		trim(name);
		trim(value);
		if (name[0] && value[0]) {
			fn(name, value, user);
			n++;
		}
	}
	return n;
}

static void load_one(const char *name, const char *value, void *user) {
	(void)user;
	put(name, value);
}

void keys_load(void) {
	char *text;
	pthread_mutex_lock(&lock);
	key_count = 0;
	if (fs_read_file(KEYS_PATH, &text, NULL) == 0) {
		each_line(text, load_one, NULL);
		free(text);
	}
	pthread_mutex_unlock(&lock);
}

void keys_get(const char *name, char *out, size_t size) {
	pthread_mutex_lock(&lock);
	Key *k = find(name);
	snprintf(out, size, "%s", k ? k->value : "");
	pthread_mutex_unlock(&lock);
}

int keys_has(const char *name) {
	pthread_mutex_lock(&lock);
	int has = find(name) != NULL;
	pthread_mutex_unlock(&lock);
	return has;
}

void keys_set(const char *name, const char *value) {
	char clean[KEY_VALUE_MAX];
	snprintf(clean, sizeof(clean), "%s", value ? value : "");
	trim(clean);
	pthread_mutex_lock(&lock);
	put(name, clean);
	save();
	pthread_mutex_unlock(&lock);
}

typedef struct {
	const char *const *known;
	int set;
} Import;

static void import_one(const char *name, const char *value, void *user) {
	Import *im = user;
	for (const char *const *k = im->known; *k; k++)
		if (strcmp(*k, name) == 0) {
			put(name, value);
			im->set++;
		}
}

int keys_import(const char *text, const char *const *known) {
	size_t n = strlen(text);
	char *copy = malloc(n + 1);
	if (!copy)
		return 0;
	memcpy(copy, text, n + 1);
	Import im = {known, 0};
	pthread_mutex_lock(&lock);
	each_line(copy, import_one, &im);
	if (im.set)
		save();
	pthread_mutex_unlock(&lock);
	free(copy);
	return im.set;
}
