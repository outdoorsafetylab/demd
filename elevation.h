#ifndef ELEVATION_H
#define ELEVATION_H

#include <stddef.h>
#include <sys/queue.h>

struct evhttp_request;
struct event;
struct context;
struct pool;

// What elevation_request_cb() is registered with. `ctx` answers lookups on the
// event loop thread itself; with `pool` set, lookups go to its workers instead,
// and the loop thread reads nothing of `ctx` but its auth, which never changes
// after creation -- `ctx` then belongs to a worker.
struct elevation_server {
    struct context *ctx;
    struct pool *pool;
};

// One request's work, from the moment the loop thread has read its body to the
// moment the reply is sent. Everything between ElevationCompute() and
// ElevationFinish() may run on another thread, so the request itself is only
// touched on the loop thread.
struct elevation_job {
    struct evhttp_request *req;
    char *body;
    size_t len;
    int detail;
    // Set on the loop thread when the client closes its connection, and read
    // by the worker between points: accessed through __atomic builtins only.
    int cancelled;
    // Filled in by ElevationCompute(). 0 means the lookup was abandoned
    // because the client went away.
    int status;
    char *reply;
    size_t reply_len;
    // The pool's watch on the client's socket while a worker has the job.
    struct event *watch;
    TAILQ_ENTRY(elevation_job) entry;
};

void elevation_request_cb(struct evhttp_request *req, void *arg);

// Parses the body, looks every point up and serialises the reply into the job.
// Touches no libevent state, so it is safe to run on any thread as long as no
// other thread is using `ctx` at the same time.
void ElevationCompute(struct context *ctx, struct elevation_job *job);

// Sends the reply ElevationCompute() prepared and frees the job. Loop thread
// only.
void ElevationFinish(struct elevation_job *job);

// Replies with `status` without computing anything, and frees the job. Loop
// thread only. Used for work that never reached a worker.
void ElevationAbort(struct elevation_job *job, int status);

#endif // ELEVATION_H
