// Reading QR codes with a camera (API keys, for one). Main thread only.
#pragma once

#include <stddef.h>
#include <vita2d.h>

#define QRSCAN_W 640
#define QRSCAN_H 480

// front: the front camera instead of the back one. 0 on success; on failure
// err says why (a PS TV has no camera, say).
int qrscan_open(int front, char *err, size_t err_size);
void qrscan_close(void);
// Call once per frame: 1 when a code was read (its text in out), else 0.
int qrscan_poll(char *out, size_t size);
// The live picture (QRSCAN_W x QRSCAN_H), or NULL before the first frame.
vita2d_texture *qrscan_preview(void);
