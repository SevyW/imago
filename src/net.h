// HTTP on top of libcurl. Each thread owns its own NetClient so connections
// are kept alive and reused between requests.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
	NET_OK = 0,
	NET_ERR = -1,
	NET_CANCELED = -2,
	NET_HTTP = -3,
	NET_WRITE = -4,
};

typedef struct NetClient NetClient;

int net_init(void);
void net_shutdown(void);
int net_online(void);

NetClient *net_client_new(void);
void net_client_free(NetClient *client);
const char *net_error(const NetClient *client);

// Fetches url into a malloc'd, NUL-terminated buffer. If stop_marker is set,
// the transfer ends successfully as soon as the marker has been received.
int net_get(NetClient *client, const char *url, char **data, size_t *size, size_t max_bytes,
            const char *stop_marker, volatile int *cancel);

// For web APIs: extra headers ("Name: value", NULL-terminated) and an optional
// POST body. On NET_HTTP the response body is still returned (APIs explain
// their errors in it); free it either way.
typedef struct {
	const char *const *headers;
	const char *post; // NULL for GET
} NetRequest;
int net_fetch(NetClient *client, const char *url, const NetRequest *req, char **data, size_t *size,
              size_t max_bytes, volatile int *cancel);
// The last response's HTTP status (0 if none arrived).
long net_status(const NetClient *client);

// Return nonzero to cancel. total is -1 when unknown.
typedef int (*NetProgress)(void *user, int64_t done, int64_t total);
int net_download(NetClient *client, const char *url, const char *path, NetProgress progress, void *user);
