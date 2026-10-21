#include "jobs.h"

#include <pthread.h>
#include <stdlib.h>

#define MAX_THREADS 4
#define STACK_SIZE (512 * 1024)

typedef struct Job {
	JobWork work;
	JobFn done;
	void *ctx;
	struct Job *next;
} Job;

typedef struct {
	Job *head, *tail;
} JobList;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static JobList pending, finished;
static pthread_t threads[MAX_THREADS];
static int thread_count;
static int stopping;

static void push(JobList *list, Job *job) {
	job->next = NULL;
	if (list->tail)
		list->tail->next = job;
	else
		list->head = job;
	list->tail = job;
}

static Job *pop(JobList *list) {
	Job *job = list->head;
	if (job) {
		list->head = job->next;
		if (!list->head)
			list->tail = NULL;
	}
	return job;
}

static void *worker(void *arg) {
	(void)arg;
	NetClient *net = net_client_new();
	pthread_mutex_lock(&lock);
	while (!stopping) {
		Job *job = pop(&pending);
		if (!job) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		pthread_mutex_unlock(&lock);
		if (job->work)
			job->work(job->ctx, net);
		pthread_mutex_lock(&lock);
		push(&finished, job);
	}
	pthread_mutex_unlock(&lock);
	net_client_free(net);
	return NULL;
}

int jobs_spawn_thread(void *(*fn)(void *), void *arg, void *thread_out) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, STACK_SIZE);
	int rc = pthread_create((pthread_t *)thread_out, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	return rc;
}

void jobs_init(int count) {
	for (int i = 0; i < count && i < MAX_THREADS; i++)
		if (jobs_spawn_thread(worker, NULL, &threads[thread_count]) == 0)
			thread_count++;
}

void jobs_shutdown(void) {
	pthread_mutex_lock(&lock);
	stopping = 1;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < thread_count; i++)
		pthread_join(threads[i], NULL);
	thread_count = 0;
}

void jobs_submit(JobWork work, JobFn done, void *ctx) {
	Job *job = malloc(sizeof(*job));
	if (!job)
		return;
	job->work = work;
	job->done = done;
	job->ctx = ctx;
	pthread_mutex_lock(&lock);
	push(&pending, job);
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void jobs_poll(void) {
	for (;;) {
		pthread_mutex_lock(&lock);
		Job *job = pop(&finished);
		pthread_mutex_unlock(&lock);
		if (!job)
			break;
		if (job->done)
			job->done(job->ctx);
		free(job);
	}
}
