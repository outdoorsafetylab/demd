#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/bufferevent.h>

#include "context.h"
#include "elevation.h"
#include "pool.h"

TAILQ_HEAD(job_list, elevation_job);

struct worker {
    struct pool *pool;
    struct context *ctx;
    pthread_t thread;
    int started;
    // The job this worker is computing, so shutdown can tell it to stop.
    // Guarded by pool->lock.
    struct elevation_job *current;
};

struct pool {
    struct event_base *base;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    // Both queues are guarded by `lock`. `pending` is filled by the loop thread
    // and drained by workers; `done` the other way round.
    struct job_list pending;
    struct job_list done;
    int stopping;
    // Activated by a worker when it moves a job to `done`; runs poolDrain() on
    // the loop thread.
    struct event *wakeup;
    struct worker *workers;
    size_t n;
};

static void *poolWork(void *arg);
static void poolDrain(evutil_socket_t fd, short events, void *arg);
static void poolPeek(evutil_socket_t fd, short events, void *arg);
static void poolRelease(struct elevation_job *job);

struct pool *PoolCreate(struct event_base *base, struct context **contexts, size_t n) {
    struct pool *pool = (struct pool *) calloc(1, sizeof(struct pool));
    if (!pool) {
        fprintf(stderr, "Failed to allocate pool: %s\n", strerror(errno));
        for (size_t i = 0; i < n; i++) {
            ContextFree(contexts[i]);
        }
        return NULL;
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->ready, NULL);
    TAILQ_INIT(&pool->pending);
    TAILQ_INIT(&pool->done);
    pool->base = base;
    pool->n = n;
    pool->workers = (struct worker *) calloc(n, sizeof(struct worker));
    if (!pool->workers) {
        fprintf(stderr, "Failed to allocate %zu worker(s): %s\n", n, strerror(errno));
        for (size_t i = 0; i < n; i++) {
            ContextFree(contexts[i]);
        }
        pool->n = 0;
        PoolFree(pool);
        return NULL;
    }
    // Handed over before anything can fail, so PoolFree() is the one place
    // that frees them whatever happens below.
    for (size_t i = 0; i < n; i++) {
        pool->workers[i].pool = pool;
        pool->workers[i].ctx = contexts[i];
    }
    pool->wakeup = event_new(base, -1, 0, poolDrain, pool);
    if (!pool->wakeup) {
        fprintf(stderr, "Failed to create pool wakeup event: %s\n", strerror(errno));
        PoolFree(pool);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        int err = pthread_create(&pool->workers[i].thread, NULL, poolWork, &pool->workers[i]);
        if (err) {
            fprintf(stderr, "Failed to start worker %zu: %s\n", i, strerror(err));
            PoolFree(pool);
            return NULL;
        }
        pool->workers[i].started = 1;
    }
    return pool;
}

void PoolSubmit(struct pool *pool, struct elevation_job *job) {
    // The only way to learn that the client gave up: a proxy that enforces a
    // timeout closes the connection, and without this the worker would go on
    // computing a reply no one will read, with every request behind it waiting.
    //
    // evhttp cannot tell: it stops reading an incoming connection once the
    // request is in, until the reply is sent, so its close callback never runs
    // for a client that leaves in between. So the socket is watched here, and
    // only peeked at -- whatever is read belongs to evhttp.
    struct evhttp_connection *evcon = evhttp_request_get_connection(job->req);
    struct bufferevent *bev = evcon ? evhttp_connection_get_bufferevent(evcon) : NULL;
    evutil_socket_t fd = bev ? bufferevent_getfd(bev) : -1;
    if (fd >= 0) {
        job->watch = event_new(pool->base, fd, EV_READ, poolPeek, job);
        if (job->watch && event_add(job->watch, NULL) != 0) {
            event_free(job->watch);
            job->watch = NULL;
        }
    }
    pthread_mutex_lock(&pool->lock);
    TAILQ_INSERT_TAIL(&pool->pending, job, entry);
    pthread_cond_signal(&pool->ready);
    pthread_mutex_unlock(&pool->lock);
}

void PoolFree(struct pool *pool) {
    if (!pool) {
        return;
    }
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    for (size_t i = 0; i < pool->n; i++) {
        if (pool->workers[i].current) {
            __atomic_store_n(&pool->workers[i].current->cancelled, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->lock);
    for (size_t i = 0; i < pool->n; i++) {
        if (pool->workers[i].started) {
            pthread_join(pool->workers[i].thread, NULL);
        }
    }
    // Every worker has exited, so nothing else touches the queues. Whatever a
    // worker finished still gets its reply; whatever none reached is refused.
    struct elevation_job *job;
    while ((job = TAILQ_FIRST(&pool->done)) != NULL) {
        TAILQ_REMOVE(&pool->done, job, entry);
        poolRelease(job);
        ElevationFinish(job);
    }
    while ((job = TAILQ_FIRST(&pool->pending)) != NULL) {
        TAILQ_REMOVE(&pool->pending, job, entry);
        poolRelease(job);
        ElevationAbort(job, 503);
    }
    if (pool->wakeup) {
        event_free(pool->wakeup);
    }
    for (size_t i = 0; i < pool->n; i++) {
        ContextFree(pool->workers[i].ctx);
    }
    free(pool->workers);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

void *poolWork(void *arg) {
    struct worker *worker = (struct worker *) arg;
    struct pool *pool = worker->pool;
    pthread_mutex_lock(&pool->lock);
    for (;;) {
        while (!pool->stopping && TAILQ_EMPTY(&pool->pending)) {
            pthread_cond_wait(&pool->ready, &pool->lock);
        }
        if (pool->stopping) {
            break;
        }
        struct elevation_job *job = TAILQ_FIRST(&pool->pending);
        TAILQ_REMOVE(&pool->pending, job, entry);
        worker->current = job;
        pthread_mutex_unlock(&pool->lock);

        ElevationCompute(worker->ctx, job);

        pthread_mutex_lock(&pool->lock);
        worker->current = NULL;
        TAILQ_INSERT_TAIL(&pool->done, job, entry);
        // Several workers may finish before the loop gets round to draining;
        // one activation drains them all, and activating an already active
        // event is harmless.
        event_active(pool->wakeup, 0, 0);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

void poolDrain(evutil_socket_t fd, short events, void *arg) {
    (void) fd;
    (void) events;
    struct pool *pool = (struct pool *) arg;
    struct job_list ready;
    TAILQ_INIT(&ready);
    pthread_mutex_lock(&pool->lock);
    TAILQ_CONCAT(&ready, &pool->done, entry);
    pthread_mutex_unlock(&pool->lock);
    struct elevation_job *job;
    while ((job = TAILQ_FIRST(&ready)) != NULL) {
        TAILQ_REMOVE(&ready, job, entry);
        poolRelease(job);
        ElevationFinish(job);
    }
}

// Loop thread. The socket turned readable while its request is being worked
// on: either the client closed it, or it sent more. Only the first means no
// one is waiting; a pipelined request is evhttp's to read once this reply is
// out, so the watch just ends. It is not persistent, so it fires once either
// way.
void poolPeek(evutil_socket_t fd, short events, void *arg) {
    (void) events;
    struct elevation_job *job = (struct elevation_job *) arg;
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        __atomic_store_n(&job->cancelled, 1, __ATOMIC_RELAXED);
    }
}

// Stops watching the socket before the reply goes out: the connection
// outlives the job when it is kept alive, and a later event must not reach a
// job that has been freed.
void poolRelease(struct elevation_job *job) {
    if (job->watch) {
        event_free(job->watch);
        job->watch = NULL;
    }
}
