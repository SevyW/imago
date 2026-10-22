#include "net.h"

#include <curl/curl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_AGENT "Imago/1.0 (PlayStation Vita)"
#define MAX_DOWNLOAD_BYTES (128LL * 1024 * 1024) // far above any real theme

struct NetClient {
	CURL *curl;
	char error[CURL_ERROR_SIZE + 64];
};

static char net_memory[1024 * 1024];

int net_init(void) {
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	SceNetInitParam param = {net_memory, sizeof(net_memory), 0};
	sceNetInit(&param); // fails harmlessly if already initialised
	sceNetCtlInit();
	return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK ? 0 : -1;
}

void net_shutdown(void) {
	curl_global_cleanup();
	sceNetCtlTerm();
	sceNetTerm();
	sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

int net_online(void) {
	int state = 0;
	return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

NetClient *net_client_new(void) {
	NetClient *c = calloc(1, sizeof(*c));
	if (c)
		c->curl = curl_easy_init();
	if (!c || !c->curl) {
		free(c);
		return NULL;
	}
	return c;
}

void net_client_free(NetClient *client) {
	if (!client)
		return;
	curl_easy_cleanup(client->curl);
	free(client);
}

const char *net_error(const NetClient *client) {
	return client ? client->error : "The network isn't available.";
}

static void setup(NetClient *c, const char *url) {
	CURL *h = c->curl;
	curl_easy_reset(h);
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // gzip: list pages shrink ~4x
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 32L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
	// psvt.ovh is used over plain HTTP; if a redirect lands on https there is
	// no current CA store on the Vita to verify against, so don't fail on it.
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, c->error);
	c->error[0] = '\0';
}

static int finish(NetClient *c, CURLcode res, int canceled) {
	if (canceled)
		return NET_CANCELED;
	if (res != CURLE_OK) {
		if (!c->error[0])
			snprintf(c->error, sizeof(c->error), "%s", curl_easy_strerror(res));
		return NET_ERR;
	}
	long status = 0;
	curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status >= 400) {
		snprintf(c->error, sizeof(c->error), "The server returned HTTP %ld", status);
		return NET_HTTP;
	}
	return NET_OK;
}

typedef struct {
	char *data;
	size_t size, capacity, max;
	const char *marker;
	int marker_found;
	volatile int *cancel;
} MemSink;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *user) {
	MemSink *s = user;
	size_t n = size * nmemb;
	if ((s->cancel && *s->cancel) || s->size + n > s->max)
		return 0;
	if (s->size + n + 1 > s->capacity) {
		size_t cap = s->capacity ? s->capacity : 64 * 1024;
		while (cap < s->size + n + 1)
			cap *= 2;
		char *grown = realloc(s->data, cap);
		if (!grown)
			return 0;
		s->data = grown;
		s->capacity = cap;
	}
	size_t from = s->size;
	memcpy(s->data + s->size, ptr, n);
	s->size += n;
	s->data[s->size] = '\0';
	if (s->marker) {
		size_t back = strlen(s->marker);
		from = from > back ? from - back : 0;
		if (strstr(s->data + from, s->marker)) {
			s->marker_found = 1;
			return 0; // abort the transfer: we have everything we need
		}
	}
	return n;
}

static int fetch(NetClient *c, const char *url, const NetRequest *req, char **data, size_t *size, size_t max_bytes,
                 const char *stop_marker, volatile int *cancel) {
	if (!c)
		return NET_ERR;
	MemSink sink = {NULL, 0, 0, max_bytes, stop_marker, 0, cancel};
	setup(c, url);
	curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, mem_write);
	curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &sink);
	struct curl_slist *headers = NULL;
	if (req) {
		for (const char *const *h = req->headers; h && *h; h++)
			headers = curl_slist_append(headers, *h);
		if (headers)
			curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, headers);
		if (req->post)
			curl_easy_setopt(c->curl, CURLOPT_POSTFIELDS, req->post);
	}
	CURLcode res = curl_easy_perform(c->curl);
	curl_slist_free_all(headers);
	if (sink.marker_found)
		res = CURLE_OK;
	else if (res == CURLE_WRITE_ERROR && sink.size >= max_bytes && !(cancel && *cancel))
		snprintf(c->error, sizeof(c->error), "Response too large");
	int rc = finish(c, res, cancel && *cancel);
	if ((rc == NET_OK || (rc == NET_HTTP && req)) && !sink.data) {
		sink.data = calloc(1, 1);
		if (!sink.data)
			rc = NET_ERR;
	}
	if (rc != NET_OK && !(rc == NET_HTTP && req)) {
		free(sink.data);
		return rc;
	}
	*data = sink.data;
	if (size)
		*size = sink.size;
	return rc;
}

int net_get(NetClient *c, const char *url, char **data, size_t *size, size_t max_bytes,
            const char *stop_marker, volatile int *cancel) {
	return fetch(c, url, NULL, data, size, max_bytes, stop_marker, cancel);
}

int net_fetch(NetClient *c, const char *url, const NetRequest *req, char **data, size_t *size, size_t max_bytes,
              volatile int *cancel) {
	static const NetRequest plain = {NULL, NULL};
	return fetch(c, url, req ? req : &plain, data, size, max_bytes, NULL, cancel);
}

long net_status(const NetClient *c) {
	long status = 0;
	if (c)
		curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
	return status;
}

typedef struct {
	FILE *file;
	int write_failed;
	NetProgress progress;
	void *user;
	int canceled;
} FileSink;

static size_t file_write(char *ptr, size_t size, size_t nmemb, void *user) {
	FileSink *s = user;
	size_t n = fwrite(ptr, size, nmemb, s->file);
	if (n != nmemb)
		s->write_failed = 1;
	return n * size;
}

static int file_progress(void *user, curl_off_t total, curl_off_t done, curl_off_t ut, curl_off_t un) {
	(void)ut;
	(void)un;
	FileSink *s = user;
	if (s->progress && s->progress(s->user, done, total > 0 ? total : -1)) {
		s->canceled = 1;
		return 1;
	}
	return 0;
}

int net_download(NetClient *c, const char *url, const char *path, NetProgress progress, void *user) {
	if (!c)
		return NET_ERR;
	FileSink sink = {fopen(path, "wb"), 0, progress, user, 0};
	if (!sink.file) {
		snprintf(c->error, sizeof(c->error), "Could not create %s", path);
		return NET_WRITE;
	}
	setup(c, url);
	curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, file_write);
	curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(c->curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c->curl, CURLOPT_XFERINFOFUNCTION, file_progress);
	curl_easy_setopt(c->curl, CURLOPT_XFERINFODATA, &sink);
	curl_easy_setopt(c->curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
	curl_easy_setopt(c->curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_DOWNLOAD_BYTES);
	CURLcode res = curl_easy_perform(c->curl);
	if (fclose(sink.file) != 0)
		sink.write_failed = 1;
	int rc = finish(c, res, sink.canceled);
	if (rc == NET_OK && sink.write_failed) {
		snprintf(c->error, sizeof(c->error), "Could not write to the memory card");
		rc = NET_WRITE;
	}
	if (rc != NET_OK)
		remove(path);
	return rc;
}
