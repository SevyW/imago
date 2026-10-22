#include "artwork.h"

#include "cJSON.h"
#include "keys.h"
#include "log.h"

#include <ctype.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_JSON (8 * 1024 * 1024)
#define GAMES_PER_SEARCH 3 // images are listed for this many of the best-matching games

// --- shared helpers --------------------------------------------------------------

static void url_encode(const char *in, char *out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < size; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			out[n++] = (char)*p;
		} else {
			out[n++] = '%';
			out[n++] = hex[*p >> 4];
			out[n++] = hex[*p & 15];
		}
	}
	out[n] = '\0';
}

// App titles carry line breaks and trademark signs that only confuse searches.
static void clean_title(const char *in, char *out, size_t size) {
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < size; p++) {
		if (p[0] == 0xE2 && p[1] == 0x84 && p[2] == 0xA2) { // ™
			p += 2;
			continue;
		}
		if (p[0] == 0xC2 && (p[1] == 0xAE || p[1] == 0xA9)) { // ® ©
			p += 1;
			continue;
		}
		char c = *p < 0x20 || *p == '"' ? ' ' : (char)*p;
		if (c == ' ' && (n == 0 || out[n - 1] == ' '))
			continue;
		out[n++] = c;
	}
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

static int contains_ci(const char *text, const char *part) {
	size_t n = strlen(part);
	for (; *text; text++)
		if (strncasecmp(text, part, n) == 0)
			return 1;
	return 0;
}

static const char *jstr(const cJSON *o, const char *key) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int jint(const cJSON *o, const char *key) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) ? v->valueint : 0;
}

static int full(const ArtResults *out) {
	return out->count >= ART_MAX_RESULTS;
}

static void add(ArtResults *out, const char *thumb, const char *full_url, const char *label, int w, int h) {
	if (full(out) || !full_url[0])
		return;
	for (int i = 0; i < out->count; i++)
		if (strcmp(out->items[i].full, full_url) == 0)
			return; // the same picture twice
	ArtResult *r = &out->items[out->count++];
	snprintf(r->thumb, sizeof(r->thumb), "%s", thumb && thumb[0] ? thumb : full_url);
	snprintf(r->full, sizeof(r->full), "%s", full_url);
	snprintf(r->label, sizeof(r->label), "%s", label);
	r->w = w;
	r->h = h;
}

// Turns a failed request into a sentence. Error bodies often say why.
static void describe(NetClient *net, int rc, const char *body, const char *who, int keyed, char *err, size_t size) {
	if (rc == NET_HTTP) {
		long status = net_status(net);
		char why[120] = "";
		cJSON *json = body ? cJSON_Parse(body) : NULL;
		if (json) {
			const char *m = jstr(json, "message");
			if (!m[0])
				m = jstr(json, "error");
			if (!m[0])
				m = jstr(json, "status");
			const cJSON *errors = cJSON_GetObjectItemCaseSensitive(json, "errors");
			if (!m[0] && cJSON_IsArray(errors) && cJSON_IsString(cJSON_GetArrayItem(errors, 0)))
				m = cJSON_GetArrayItem(errors, 0)->valuestring;
			snprintf(why, sizeof(why), "%s", m);
			cJSON_Delete(json);
		} else if (body) {
			snprintf(why, sizeof(why), "%.100s", body); // ScreenScraper answers in plain text
		}
		if (status == 429 || status == 430)
			snprintf(err, size, "%s says there were too many requests. Wait a minute and try again.", who);
		else if ((status == 401 || status == 403) && keyed)
			snprintf(err, size, "%s refused the key (HTTP %ld)%s%s", who, status, why[0] ? ": " : ".", why);
		else
			snprintf(err, size, "%s answered with an error (HTTP %ld)%s%s", who, status, why[0] ? ": " : ".", why);
	} else if (!net_online()) {
		snprintf(err, size, "The PS Vita isn't connected to the internet.");
	} else {
		snprintf(err, size, "Couldn't reach %s: %s", who, net_error(net));
	}
}

// GETs (or POSTs) url and parses the JSON answer.
static cJSON *fetch_json(NetClient *net, const char *url, const NetRequest *req, const char *who, int keyed, char *err,
                         size_t size) {
	char *body = NULL;
	int rc = net_fetch(net, url, req, &body, NULL, MAX_JSON, NULL);
	if (rc != NET_OK) {
		describe(net, rc, body, who, keyed, err, size);
		free(body);
		return NULL;
	}
	cJSON *json = cJSON_Parse(body);
	free(body);
	if (!json)
		snprintf(err, size, "%s sent something Imago couldn't read.", who);
	return json;
}

// --- VitaDB: homebrew, matched by title ID -----------------------------------------

#define VITADB "https://www.rinnegatamante.eu/vitadb/"

typedef struct {
	char titleid[16];
	char name[96];
	char icon[80];
	char shots[320]; // paths separated by ';'
} VdbEntry;

static VdbEntry *vdb;
static int vdb_count;
static pthread_mutex_t vdb_lock = PTHREAD_MUTEX_INITIALIZER;

// The list is about 2 MB: fetched once per session, kept in a compact form.
static int vdb_load(NetClient *net, int force, char *err, size_t size) {
	pthread_mutex_lock(&vdb_lock);
	if (vdb && !force) {
		pthread_mutex_unlock(&vdb_lock);
		return 0;
	}
	// The server answers an empty list to unknown clients; this is what
	// VitaDB's own downloader sends.
	static const char *const headers[] = {"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
	                                      "(KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36",
	                                      NULL};
	NetRequest req = {headers, NULL};
	cJSON *json = fetch_json(net, VITADB "list_hbs_json.php", &req, "VitaDB", 0, err, size);
	int n = cJSON_GetArraySize(json);
	VdbEntry *list = n > 0 ? calloc((size_t)n, sizeof(VdbEntry)) : NULL;
	int count = 0;
	const cJSON *it;
	if (list)
		cJSON_ArrayForEach(it, json) {
			VdbEntry *e = &list[count];
			snprintf(e->titleid, sizeof(e->titleid), "%s", jstr(it, "titleid"));
			snprintf(e->name, sizeof(e->name), "%s", jstr(it, "name"));
			snprintf(e->icon, sizeof(e->icon), "%s", jstr(it, "icon"));
			snprintf(e->shots, sizeof(e->shots), "%s", jstr(it, "screenshots"));
			if (e->name[0])
				count++;
		}
	cJSON_Delete(json);
	if (json && !count) {
		free(list);
		list = NULL;
		snprintf(err, size, "VitaDB sent an empty list.");
	}
	if (list) {
		free(vdb);
		vdb = list;
		vdb_count = count;
	}
	int rc = vdb ? 0 : -1;
	pthread_mutex_unlock(&vdb_lock);
	return rc;
}

static void vdb_add(const VdbEntry *e, ArtPart part, ArtResults *out) {
	char url[400], label[96];
	if (part == ART_ICON && e->icon[0]) {
		snprintf(url, sizeof(url), VITADB "icons/%s", e->icon);
		snprintf(label, sizeof(label), "%s \xC2\xB7 icon", e->name);
		add(out, NULL, url, label, 128, 128);
	}
	char shots[sizeof(e->shots)], *rest;
	snprintf(shots, sizeof(shots), "%s", e->shots);
	for (char *s = strtok_r(shots, ";", &rest); s; s = strtok_r(NULL, ";", &rest)) {
		snprintf(url, sizeof(url), VITADB "%s", s);
		snprintf(label, sizeof(label), "%s \xC2\xB7 screenshot", e->name);
		add(out, NULL, url, label, 960, 544);
	}
}

static int vitadb_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	if (vdb_load(net, 0, err, size) != 0)
		return -1;
	pthread_mutex_lock(&vdb_lock);
	for (int i = 0; i < vdb_count; i++) // this very app first
		if (q->title_id[0] && strcasecmp(vdb[i].titleid, q->title_id) == 0)
			vdb_add(&vdb[i], q->part, out);
	char title[128];
	clean_title(q->title, title, sizeof(title));
	int matches = 0;
	for (int i = 0; i < vdb_count && matches < 8 && title[0]; i++)
		if (strcasecmp(vdb[i].titleid, q->title_id) != 0 && contains_ci(vdb[i].name, title)) {
			vdb_add(&vdb[i], q->part, out);
			matches++;
		}
	pthread_mutex_unlock(&vdb_lock);
	return 0;
}

static int vitadb_test(NetClient *net, char *err, size_t size) {
	return vdb_load(net, 1, err, size);
}

// --- PlayStation Store: official icons of retail games ------------------------------

#define PS_STORE "https://store.playstation.com/store/api/chihiro/00_09_000/tumbler/US/en/999/"

static const char *store_image(const cJSON *images, int type) {
	const cJSON *im;
	cJSON_ArrayForEach(im, images) if (jint(im, "type") == type) return jstr(im, "url");
	return "";
}

static int store_is_vita(const cJSON *link) {
	const cJSON *p;
	cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(link, "playable_platform")) if (
	    cJSON_IsString(p) && strstr(p->valuestring, "Vita")) return 1;
	return 0;
}

static void store_add(const cJSON *link, ArtResults *out) {
	const cJSON *images = cJSON_GetObjectItemCaseSensitive(link, "images");
	const char *big = store_image(images, 10), *small = store_image(images, 1);
	if (!big[0])
		big = small;
	add(out, small, big, jstr(link, "name"), big == small ? 240 : 1024, big == small ? 240 : 1024);
}

static int psstore_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char title[128], enc[400], url[600];
	clean_title(q->title, title, sizeof(title));
	url_encode(title, enc, sizeof(enc));
	snprintf(url, sizeof(url), PS_STORE "%s?suggested_size=20&mode=game", enc);
	cJSON *json = fetch_json(net, url, NULL, "The PlayStation Store", 0, err, size);
	if (!json)
		return -1;
	const cJSON *links = cJSON_GetObjectItemCaseSensitive(json, "links"), *link;
	// This very game (its title ID is part of the content ID) first, then other Vita games.
	cJSON_ArrayForEach(link, links) if (q->title_id[0] && strstr(jstr(link, "id"), q->title_id)) store_add(link, out);
	cJSON_ArrayForEach(link, links) if (store_is_vita(link)) store_add(link, out);
	cJSON_Delete(json);
	return 0;
}

static int psstore_test(NetClient *net, char *err, size_t size) {
	cJSON *json = fetch_json(net, PS_STORE "Gravity%20Rush?suggested_size=1&mode=game", NULL, "The PlayStation Store", 0,
	                         err, size);
	cJSON_Delete(json);
	return json ? 0 : -1;
}

// --- SteamGridDB -------------------------------------------------------------------

#define SGDB "https://www.steamgriddb.com/api/v2/"

static int sgdb_request(NetClient *net, const char *path, cJSON **out_json, char *err, size_t size) {
	char key[KEY_VALUE_MAX], auth[KEY_VALUE_MAX + 32], url[600];
	keys_get("steamgriddb", key, sizeof(key));
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
	const char *headers[] = {auth, NULL};
	NetRequest req = {headers, NULL};
	snprintf(url, sizeof(url), SGDB "%s", path);
	*out_json = fetch_json(net, url, &req, "SteamGridDB", 1, err, size);
	return *out_json ? 0 : -1;
}

static int has_ext(const char *url, const char *ext) {
	size_t n = strlen(url), e = strlen(ext);
	return n >= e && strcasecmp(url + n - e, ext) == 0;
}

static void sgdb_images(NetClient *net, int game, const char *game_name, const char *kind, const char *filters,
                        ArtResults *out) {
	char path[300], err[ART_ERROR_SIZE];
	snprintf(path, sizeof(path), "%s/game/%d?%s&nsfw=false&limit=20", kind, game, filters);
	cJSON *json;
	if (sgdb_request(net, path, &json, err, sizeof(err)) != 0) {
		log_write("online: steamgriddb %s: %s", kind, err);
		return;
	}
	const cJSON *it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(json, "data")) {
		const char *url = jstr(it, "url"), *thumb = jstr(it, "thumb");
		if (!has_ext(url, ".png") && !has_ext(url, ".jpg") && !has_ext(url, ".jpeg"))
			continue;
		if (!has_ext(thumb, ".png") && !has_ext(thumb, ".jpg"))
			thumb = url; // WebP thumbnails can't be shown
		add(out, thumb, url, game_name, jint(it, "width"), jint(it, "height"));
	}
	cJSON_Delete(json);
}

static int sgdb_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char title[128], enc[400], path[500];
	clean_title(q->title, title, sizeof(title));
	url_encode(title, enc, sizeof(enc));
	snprintf(path, sizeof(path), "search/autocomplete/%s", enc);
	cJSON *json;
	if (sgdb_request(net, path, &json, err, size) != 0)
		return -1;
	const cJSON *game;
	int games = 0;
	cJSON_ArrayForEach(game, cJSON_GetObjectItemCaseSensitive(json, "data")) {
		if (games++ >= GAMES_PER_SEARCH || full(out))
			break;
		int id = jint(game, "id");
		const char *name = jstr(game, "name");
		// Pictures over 2048 pixels a side are left out: Imago can't read PNGs that big.
		if (q->part == ART_ICON) {
			sgdb_images(net, id, name, "icons", "mimes=image/png&dimensions=128,256,512,1024", out);
			sgdb_images(net, id, name, "grids", "mimes=image/png,image/jpeg&dimensions=512x512,1024x1024", out);
		} else {
			sgdb_images(net, id, name, "heroes", "mimes=image/png,image/jpeg&dimensions=1920x620,1600x650", out);
			sgdb_images(net, id, name, "grids", "mimes=image/png,image/jpeg&dimensions=920x430,460x215", out);
		}
	}
	cJSON_Delete(json);
	return 0;
}

static int sgdb_test(NetClient *net, char *err, size_t size) {
	cJSON *json;
	if (sgdb_request(net, "search/autocomplete/test", &json, err, size) != 0)
		return -1;
	cJSON_Delete(json);
	return 0;
}

// --- IGDB (through Twitch) ---------------------------------------------------------

#define IGDB "https://api.igdb.com/v4/"
#define IGDB_IMAGE "https://images.igdb.com/igdb/image/upload/"
#define IGDB_VITA 46

static char igdb_token[128], igdb_token_owner[KEY_VALUE_MAX];
static unsigned long long igdb_token_expires;
static pthread_mutex_t igdb_lock = PTHREAD_MUTEX_INITIALIZER;

// Twitch hands out app tokens for about 60 days; one per session is plenty.
static int igdb_get_token(NetClient *net, int force, char *token, size_t token_size, char *err, size_t size) {
	char id[KEY_VALUE_MAX], secret[KEY_VALUE_MAX];
	keys_get("igdb_id", id, sizeof(id));
	keys_get("igdb_secret", secret, sizeof(secret));
	unsigned long long now = sceKernelGetProcessTimeWide();
	pthread_mutex_lock(&igdb_lock);
	if (!force && igdb_token[0] && strcmp(igdb_token_owner, id) == 0 && now < igdb_token_expires) {
		snprintf(token, token_size, "%s", igdb_token);
		pthread_mutex_unlock(&igdb_lock);
		return 0;
	}
	pthread_mutex_unlock(&igdb_lock);
	char enc_id[KEY_VALUE_MAX * 3], enc_secret[KEY_VALUE_MAX * 3], body[KEY_VALUE_MAX * 6 + 64];
	url_encode(id, enc_id, sizeof(enc_id));
	url_encode(secret, enc_secret, sizeof(enc_secret));
	snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&grant_type=client_credentials", enc_id, enc_secret);
	NetRequest req = {NULL, body};
	cJSON *json = fetch_json(net, "https://id.twitch.tv/oauth2/token", &req, "Twitch", 1, err, size);
	if (!json)
		return -1;
	const char *access = jstr(json, "access_token");
	int expires = jint(json, "expires_in");
	if (!access[0]) {
		snprintf(err, size, "Twitch didn't hand out a token.");
		cJSON_Delete(json);
		return -1;
	}
	pthread_mutex_lock(&igdb_lock);
	snprintf(igdb_token, sizeof(igdb_token), "%s", access);
	snprintf(igdb_token_owner, sizeof(igdb_token_owner), "%s", id);
	igdb_token_expires = now + (unsigned long long)(expires > 3600 ? expires - 3600 : expires) * 1000000ULL;
	snprintf(token, token_size, "%s", igdb_token);
	pthread_mutex_unlock(&igdb_lock);
	cJSON_Delete(json);
	return 0;
}

static cJSON *igdb_query(NetClient *net, const char *endpoint, const char *query, char *err, size_t size) {
	char token[128];
	for (int attempt = 0; attempt < 2; attempt++) {
		if (igdb_get_token(net, attempt > 0, token, sizeof(token), err, size) != 0)
			return NULL;
		char id[KEY_VALUE_MAX], client[KEY_VALUE_MAX + 16], auth[160], url[128];
		keys_get("igdb_id", id, sizeof(id));
		snprintf(client, sizeof(client), "Client-ID: %s", id);
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
		const char *headers[] = {client, auth, "Content-Type: text/plain", NULL};
		NetRequest req = {headers, query};
		snprintf(url, sizeof(url), IGDB "%s", endpoint);
		cJSON *json = fetch_json(net, url, &req, "IGDB", 1, err, size);
		if (json || net_status(net) != 401)
			return json;
		// 401: the token went stale; get a new one once.
	}
	return NULL;
}

static void igdb_add(const cJSON *image, const char *thumb_size, const char *full_size, const char *label,
                     ArtResults *out) {
	const char *id = jstr(image, "image_id");
	if (!id[0])
		return;
	char thumb[200], full_url[200];
	snprintf(thumb, sizeof(thumb), IGDB_IMAGE "t_%s/%s.jpg", thumb_size, id);
	snprintf(full_url, sizeof(full_url), IGDB_IMAGE "t_%s/%s.jpg", full_size, id);
	add(out, thumb, full_url, label, jint(image, "width"), jint(image, "height"));
}

static int igdb_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char title[128], query[512];
	clean_title(q->title, title, sizeof(title));
	cJSON *json = NULL;
	// Vita releases first; if there are none, any platform.
	for (int any = 0; any < 2 && (!json || !cJSON_GetArraySize(json)); any++) {
		cJSON_Delete(json);
		char where[48] = "";
		if (!any)
			snprintf(where, sizeof(where), " where platforms = (%d);", IGDB_VITA);
		snprintf(query, sizeof(query),
		         "search \"%s\"; fields name,cover.image_id,cover.width,cover.height,artworks.image_id,"
		         "artworks.width,artworks.height,screenshots.image_id,screenshots.width,screenshots.height;%s limit %d;",
		         title, where, GAMES_PER_SEARCH);
		json = igdb_query(net, "games", query, err, size);
		if (!json)
			return -1;
	}
	const cJSON *game;
	cJSON_ArrayForEach(game, json) {
		const char *name = jstr(game, "name");
		const cJSON *cover = cJSON_GetObjectItemCaseSensitive(game, "cover"), *im;
		const cJSON *artworks = cJSON_GetObjectItemCaseSensitive(game, "artworks");
		const cJSON *shots = cJSON_GetObjectItemCaseSensitive(game, "screenshots");
		if (q->part == ART_ICON && cover)
			igdb_add(cover, "cover_big", "cover_big_2x", name, out);
		cJSON_ArrayForEach(im, artworks) igdb_add(im, "screenshot_med", "1080p", name, out);
		cJSON_ArrayForEach(im, shots) igdb_add(im, "screenshot_med", "1080p", name, out);
	}
	cJSON_Delete(json);
	return 0;
}

static int igdb_test(NetClient *net, char *err, size_t size) {
	char token[128];
	if (igdb_get_token(net, 1, token, sizeof(token), err, size) != 0)
		return -1;
	cJSON *json = igdb_query(net, "platforms", "fields name; where id = 46;", err, size);
	cJSON_Delete(json);
	return json ? 0 : -1;
}

// --- TheGamesDB --------------------------------------------------------------------

#define TGDB "https://api.thegamesdb.net/"
#define TGDB_VITA 39

static int tgdb_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char key[KEY_VALUE_MAX], enc_key[KEY_VALUE_MAX * 3], title[128], enc[400], url[900];
	keys_get("thegamesdb", key, sizeof(key));
	url_encode(key, enc_key, sizeof(enc_key));
	clean_title(q->title, title, sizeof(title));
	url_encode(title, enc, sizeof(enc));
	cJSON *json = NULL;
	const cJSON *games = NULL;
	for (int any = 0; any < 2 && cJSON_GetArraySize(games) == 0; any++) {
		cJSON_Delete(json);
		snprintf(url, sizeof(url), TGDB "v1.1/Games/ByGameName?apikey=%s&name=%s%s", enc_key, enc,
		         any ? "" : "&filter%5Bplatform%5D=39");
		json = fetch_json(net, url, NULL, "TheGamesDB", 1, err, size);
		if (!json)
			return -1;
		games = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, "data"), "games");
	}
	char ids[64] = "", names[GAMES_PER_SEARCH][96];
	int ids_n[GAMES_PER_SEARCH], n = 0;
	const cJSON *game;
	cJSON_ArrayForEach(game, games) {
		if (n >= GAMES_PER_SEARCH)
			break;
		ids_n[n] = jint(game, "id");
		snprintf(names[n], sizeof(names[0]), "%s", jstr(game, "game_title"));
		size_t len = strlen(ids);
		snprintf(ids + len, sizeof(ids) - len, "%s%d", n ? "," : "", ids_n[n]);
		n++;
	}
	cJSON_Delete(json);
	if (!n)
		return 0;
	const char *types = q->part == ART_ICON ? "boxart,clearlogo" : "fanart,screenshot,banner,titlescreen";
	snprintf(url, sizeof(url), TGDB "v1/Games/Images?apikey=%s&games_id=%s&filter%%5Btype%%5D=%s", enc_key, ids,
	         types);
	json = fetch_json(net, url, NULL, "TheGamesDB", 1, err, size);
	if (!json)
		return -1;
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	const cJSON *base = cJSON_GetObjectItemCaseSensitive(data, "base_url");
	const cJSON *images = cJSON_GetObjectItemCaseSensitive(data, "images");
	for (int g = 0; g < n; g++) {
		char id[16];
		snprintf(id, sizeof(id), "%d", ids_n[g]);
		const cJSON *im;
		cJSON_ArrayForEach(im, cJSON_GetObjectItemCaseSensitive(images, id)) {
			if (strcmp(jstr(im, "type"), "boxart") == 0 && strcmp(jstr(im, "side"), "front") != 0)
				continue; // the back of the box
			const char *file = jstr(im, "filename");
			char thumb[400], full_url[400], label[96];
			snprintf(thumb, sizeof(thumb), "%s%s", jstr(base, "thumb"), file);
			snprintf(full_url, sizeof(full_url), "%s%s", jstr(base, "original"), file);
			snprintf(label, sizeof(label), "%s \xC2\xB7 %s", names[g], jstr(im, "type"));
			int w = 0, h = 0;
			sscanf(jstr(im, "resolution"), "%dx%d", &w, &h);
			add(out, thumb, full_url, label, w, h);
		}
	}
	cJSON_Delete(json);
	return 0;
}

static int tgdb_test(NetClient *net, char *err, size_t size) {
	char key[KEY_VALUE_MAX], enc_key[KEY_VALUE_MAX * 3], url[900];
	keys_get("thegamesdb", key, sizeof(key));
	url_encode(key, enc_key, sizeof(enc_key));
	snprintf(url, sizeof(url), TGDB "v1/Platforms/ByPlatformID?apikey=%s&id=%d", enc_key, TGDB_VITA);
	cJSON *json = fetch_json(net, url, NULL, "TheGamesDB", 1, err, size);
	cJSON_Delete(json);
	return json ? 0 : -1;
}

// --- RAWG --------------------------------------------------------------------------

#define RAWG "https://api.rawg.io/api/"
#define RAWG_VITA 19

// media.rawg.io/media/games/x.jpg also comes resized: media/resize/420/-/games/x.jpg
static void rawg_thumb(const char *url, char *out, size_t size) {
	const char *at = strstr(url, "/media/");
	if (at && strncmp(at, "/media/resize/", 14) != 0)
		snprintf(out, size, "%.*s/media/resize/420/-/%s", (int)(at - url), url, at + 7);
	else
		snprintf(out, size, "%s", url);
}

static void rawg_add(const char *url, const char *label, ArtResults *out) {
	char thumb[400];
	rawg_thumb(url, thumb, sizeof(thumb));
	add(out, thumb, url, label, 0, 0);
}

static int rawg_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char key[KEY_VALUE_MAX], enc_key[KEY_VALUE_MAX * 3], title[128], enc[400], url[900];
	keys_get("rawg", key, sizeof(key));
	url_encode(key, enc_key, sizeof(enc_key));
	clean_title(q->title, title, sizeof(title));
	url_encode(title, enc, sizeof(enc));
	cJSON *json = NULL;
	const cJSON *results = NULL;
	for (int any = 0; any < 2 && cJSON_GetArraySize(results) == 0; any++) {
		cJSON_Delete(json);
		snprintf(url, sizeof(url), RAWG "games?key=%s&search=%s&page_size=%d%s", enc_key, enc, GAMES_PER_SEARCH + 2,
		         any ? "" : "&platforms=19");
		json = fetch_json(net, url, NULL, "RAWG", 1, err, size);
		if (!json)
			return -1;
		results = cJSON_GetObjectItemCaseSensitive(json, "results");
	}
	const cJSON *game, *shot;
	cJSON_ArrayForEach(game, results) {
		const char *name = jstr(game, "name");
		rawg_add(jstr(game, "background_image"), name, out);
		cJSON_ArrayForEach(shot, cJSON_GetObjectItemCaseSensitive(game, "short_screenshots"))
		    rawg_add(jstr(shot, "image"), name, out);
	}
	cJSON_Delete(json);
	return 0;
}

static int rawg_test(NetClient *net, char *err, size_t size) {
	char key[KEY_VALUE_MAX], enc_key[KEY_VALUE_MAX * 3], url[900];
	keys_get("rawg", key, sizeof(key));
	url_encode(key, enc_key, sizeof(enc_key));
	snprintf(url, sizeof(url), RAWG "platforms?key=%s&page_size=1", enc_key);
	cJSON *json = fetch_json(net, url, NULL, "RAWG", 1, err, size);
	cJSON_Delete(json);
	return json ? 0 : -1;
}

// --- ScreenScraper -----------------------------------------------------------------
// Needs developer credentials for Imago (given by ScreenScraper), set at build
// time: cmake -DIMAGO_SS_DEVID=... -DIMAGO_SS_DEVPASSWORD=...

#if defined(IMAGO_SS_DEVID) && defined(IMAGO_SS_DEVPASSWORD)
#define HAVE_SCREENSCRAPER 1
#define SS "https://api.screenscraper.fr/api2/"
#define SS_VITA 62

static void ss_credentials(char *out, size_t size) {
	char user[KEY_VALUE_MAX], password[KEY_VALUE_MAX], enc_user[KEY_VALUE_MAX * 3], enc_password[KEY_VALUE_MAX * 3];
	keys_get("screenscraper_user", user, sizeof(user));
	keys_get("screenscraper_password", password, sizeof(password));
	url_encode(user, enc_user, sizeof(enc_user));
	url_encode(password, enc_password, sizeof(enc_password));
	snprintf(out, size, "devid=%s&devpassword=%s&softname=Imago&output=json&ssid=%s&sspassword=%s", IMAGO_SS_DEVID,
	         IMAGO_SS_DEVPASSWORD, enc_user, enc_password);
}

static int ss_search(NetClient *net, const ArtQuery *q, ArtResults *out, char *err, size_t size) {
	char creds[KEY_VALUE_MAX * 7], title[128], enc[400], url[KEY_VALUE_MAX * 8];
	ss_credentials(creds, sizeof(creds));
	clean_title(q->title, title, sizeof(title));
	url_encode(title, enc, sizeof(enc));
	snprintf(url, sizeof(url), SS "jeuRecherche.php?%s&systemeid=%d&recherche=%s", creds, SS_VITA, enc);
	cJSON *json = fetch_json(net, url, NULL, "ScreenScraper", 1, err, size);
	if (!json)
		return -1;
	static const char *const icon_types[] = {"box-2D", "wheel-hd", "wheel", NULL};
	static const char *const wide_types[] = {"fanart", "ss", "sstitle", NULL};
	const char *const *types = q->part == ART_ICON ? icon_types : wide_types;
	const cJSON *games = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, "response"), "jeux");
	const cJSON *game, *media;
	int count = 0;
	cJSON_ArrayForEach(game, games) {
		if (count++ >= GAMES_PER_SEARCH)
			break;
		const char *name = jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(game, "noms"), 0), "text");
		for (int t = 0; types[t]; t++)
			cJSON_ArrayForEach(media, cJSON_GetObjectItemCaseSensitive(game, "medias")) {
				if (strcmp(jstr(media, "type"), types[t]) != 0)
					continue;
				char thumb[512], full_url[512], label[96];
				// The media URLs carry the credentials; ask for JPEGs a sensible size.
				snprintf(thumb, sizeof(thumb), "%s&maxwidth=320&outputformat=jpg", jstr(media, "url"));
				snprintf(full_url, sizeof(full_url), "%s&maxwidth=1920&outputformat=png", jstr(media, "url"));
				snprintf(label, sizeof(label), "%s \xC2\xB7 %s", name, types[t]);
				add(out, thumb, full_url, label, 0, 0);
			}
	}
	cJSON_Delete(json);
	return 0;
}

static int ss_test(NetClient *net, char *err, size_t size) {
	char creds[KEY_VALUE_MAX * 7], url[KEY_VALUE_MAX * 8];
	ss_credentials(creds, sizeof(creds));
	snprintf(url, sizeof(url), SS "ssuserInfos.php?%s", creds);
	cJSON *json = fetch_json(net, url, NULL, "ScreenScraper", 1, err, size);
	cJSON_Delete(json);
	return json ? 0 : -1;
}
#endif

// --- the list ----------------------------------------------------------------------

#define ICON (1u << ART_ICON)
#define BG (1u << ART_BG)
#define GATE (1u << ART_GATE)

static const char *const sgdb_keys[] = {"steamgriddb", NULL};
static const char *const sgdb_prompts[] = {"SteamGridDB API key", NULL};
static const char *const igdb_keys[] = {"igdb_id", "igdb_secret", NULL};
static const char *const igdb_prompts[] = {"Twitch app: Client ID", "Twitch app: Client Secret", NULL};
static const char *const tgdb_keys[] = {"thegamesdb", NULL};
static const char *const tgdb_prompts[] = {"TheGamesDB API key", NULL};
static const char *const rawg_keys[] = {"rawg", NULL};
static const char *const rawg_prompts[] = {"RAWG API key", NULL};
#ifdef HAVE_SCREENSCRAPER
static const char *const ss_keys[] = {"screenscraper_user", "screenscraper_password", NULL};
static const char *const ss_prompts[] = {"ScreenScraper user name", "ScreenScraper password", NULL};
#endif

static const ArtSource sources[] = {
    {"vitadb", "VitaDB", NULL, NULL,
     "Icons and screenshots of homebrew apps, from VitaDB (rinnegatamante.eu/vitadb). The app's own entry is found by "
     "its title ID. No key needed.",
     NULL, ICON | BG | GATE, vitadb_search, vitadb_test},
    {"psstore", "PlayStation Store", NULL, NULL,
     "Official icons of retail games, from the PlayStation Store. No key needed. The Store's search isn't a public "
     "API, so it may stop working one day.",
     NULL, ICON, psstore_search, psstore_test},
    {"steamgriddb", "SteamGridDB", sgdb_keys, sgdb_prompts,
     "Free. Sign in at steamgriddb.com, then open Preferences > API (steamgriddb.com/profile/preferences/api) and "
     "generate a key.\n\nIn a QR code: steamgriddb=YOUR_KEY",
     NULL, ICON | BG | GATE, sgdb_search, sgdb_test},
    {"igdb", "IGDB", igdb_keys, igdb_prompts,
     "Free, through Twitch. Turn on two-factor authentication on your Twitch account, then register an application at "
     "dev.twitch.tv/console/apps (OAuth redirect URL: http://localhost, client type: Confidential). Copy its Client "
     "ID and make a new Client Secret.\n\nIn a QR code, two lines: igdb_id=CLIENT_ID and igdb_secret=SECRET",
     NULL, ICON | BG | GATE, igdb_search, igdb_test},
    {"thegamesdb", "TheGamesDB", tgdb_keys, tgdb_prompts,
     "Free. Make an account at forums.thegamesdb.net and request an API key (api.thegamesdb.net/key.php). Every "
     "search uses part of a monthly allowance.\n\nIn a QR code: thegamesdb=YOUR_KEY",
     NULL, ICON | BG | GATE, tgdb_search, tgdb_test},
    {"rawg", "RAWG", rawg_keys, rawg_prompts,
     "Free for personal use. Sign up at rawg.io/apidocs and press \"Get API Key\". Backgrounds and screenshots "
     "only.\n\nIn a QR code: rawg=YOUR_KEY",
     "Data and images from RAWG (rawg.io)", BG | GATE, rawg_search, rawg_test},
#ifdef HAVE_SCREENSCRAPER
    {"screenscraper", "ScreenScraper", ss_keys, ss_prompts,
     "Free account at screenscraper.fr: enter your user name and password.\n\nIn a QR code, two lines: "
     "screenscraper_user=NAME and screenscraper_password=PASSWORD",
     "Images from ScreenScraper (screenscraper.fr)", ICON | BG | GATE, ss_search, ss_test},
#endif
};

int artwork_count(void) {
	return (int)(sizeof(sources) / sizeof(sources[0]));
}

const ArtSource *artwork_get(int index) {
	return &sources[index];
}

const ArtSource *artwork_find(const char *id) {
	for (int i = 0; i < artwork_count(); i++)
		if (strcmp(sources[i].id, id) == 0)
			return &sources[i];
	return NULL;
}

int artwork_ready(const ArtSource *s) {
	for (int i = 0; s->keys && s->keys[i]; i++)
		if (!keys_has(s->keys[i]))
			return 0;
	return 1;
}

const char *const *artwork_key_names(void) {
	static const char *names[16];
	if (!names[0]) {
		int n = 0;
		for (int i = 0; i < artwork_count(); i++)
			for (int k = 0; sources[i].keys && sources[i].keys[k] && n < 15; k++)
				names[n++] = sources[i].keys[k];
	}
	return names;
}
