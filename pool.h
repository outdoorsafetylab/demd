#ifndef POOL_H_
#define POOL_H_

#include <stddef.h>

struct event_base;
struct context;
struct elevation_job;

// Lookup workers. The event loop keeps all HTTP I/O; a worker only turns a
// request body into a reply, using its own context. A context holds GDAL
// dataset handles, and one handle cannot be read from two threads at once, so
// the contexts are never shared: `contexts` holds one per worker, and the pool
// takes ownership of them.
//
// Requires evthread_use_pthreads() before `base` was created: workers wake the
// loop with event_active(), which is only safe across threads on a base that
// has locking.
struct pool *PoolCreate(struct event_base *base, struct context **contexts, size_t n);

// Queues a job for the next idle worker. Loop thread only. The reply is sent
// from the loop thread once the worker is done.
void PoolSubmit(struct pool *pool, struct elevation_job *job);

// Stops the workers and joins them. A worker in the middle of a lookup is told
// to abandon it. Every job left is then answered, finished or not, because
// answering is what makes libevent free a request -- but the loop has stopped
// by now, so none of it reaches a client: they see the connection close. Loop
// thread only, and before evhttp_free(), while the requests still exist.
void PoolFree(struct pool *pool);

#endif // POOL_H_
