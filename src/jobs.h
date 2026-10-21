// Background work queue. `work` runs on a worker thread and must not touch UI
// state; `done` runs later on the main thread (from jobs_poll) and owns ctx.
#pragma once

#include "net.h"

// Each worker owns a NetClient so HTTP connections are reused.
typedef void (*JobWork)(void *ctx, NetClient *net);
typedef void (*JobFn)(void *ctx);

void jobs_init(int threads);
void jobs_shutdown(void);
void jobs_submit(JobWork work, JobFn done, void *ctx);
// Call once per frame on the main thread.
void jobs_poll(void);

// Creates a joinable pthread with a stack large enough for curl/libjpeg.
int jobs_spawn_thread(void *(*fn)(void *), void *arg, void *thread_out);
