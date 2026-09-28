#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>

#if JSON_C_VERSION_NUM < 0x000D00
#error "json-c 0.13 or newer is required for json_tokener_get_parse_end()"
#endif

#include "context.h"
#include "pool.h"

#include "elevation.h"

static const char *contentType = "application/json; charset=utf-8";

static int coordValue(json_object *obj, double *val);
static int detailRequested(struct evhttp_request *req, int *detail);
static void jobFree(struct elevation_job *job);

// The checks that need no body parsed happen here, on the loop thread, so a
// request that fails them never occupies a worker. Everything else -- parsing,
// the lookups, serialising -- is ElevationCompute(), which runs here too unless
// there is a pool to hand it to.
void elevation_request_cb(struct evhttp_request *req, void *arg) {
    struct elevation_server *server = (struct elevation_server *) arg;
    struct elevation_job *job = NULL;
    size_t len;
    evbuffer *input;

    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_POST:
        break;
    default:
        evhttp_send_error(req, 405, NULL);
        return;
    }

    const char *auth = ContextAuth(server->ctx);
    if (auth) {
        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
        const char *value = evhttp_find_header(headers, "Authorization");
        if (!value || strcmp(auth, value)) {
            evhttp_send_error(req, 401, NULL);
            return;
        }
    }

    // Decided before the body is read: a request naming an unknown detail level
    // is malformed however valid its coordinates are.
    int detail;
    if (!detailRequested(req, &detail)) {
        evhttp_send_error(req, 400, NULL);
        return;
    }

    input = evhttp_request_get_input_buffer(req);
    if (!input) {
        fprintf(stderr, "Failed to get input buffer: %s\n", strerror(errno));
        evhttp_send_error(req, 500, NULL);
        return;
    }

    len = evbuffer_get_length(input);
    if (len == 0) {
        evhttp_send_error(req, 400, NULL);
        return;
    }
    if (len > INT_MAX) {
        evhttp_send_error(req, 413, NULL);
        return;
    }

    job = (struct elevation_job *) calloc(1, sizeof(struct elevation_job));
    if (!job) {
        fprintf(stderr, "Failed to allocate job: %s\n", strerror(errno));
        evhttp_send_error(req, 500, NULL);
        return;
    }
    job->req = req;
    job->detail = detail;
    job->len = len;
    job->body = (char *) malloc(len);
    if (!job->body) {
        fprintf(stderr, "Failed to allocate %zu bytes for input: %s\n", len, strerror(errno));
        ElevationAbort(job, 500);
        return;
    }
    if (evbuffer_copyout(input, job->body, len) != (ev_ssize_t) len) {
        fprintf(stderr, "Failed to drain input buffer: %s\n", strerror(errno));
        ElevationAbort(job, 500);
        return;
    }

    if (server->pool) {
        PoolSubmit(server->pool, job);
    } else {
        ElevationCompute(server->ctx, job);
        ElevationFinish(job);
    }
}

void ElevationCompute(struct context *ctx, struct elevation_job *job) {
    json_object *coords, *json = NULL, *result = NULL;
    json_tokener *tok = NULL;
    size_t len = job->len, end, n, max;
    const char *data = job->body;

    // Parse with an explicit length: the buffer is not NUL-terminated, so the
    // string-oriented entry points would read past the end of the allocation.
    tok = json_tokener_new();
    if (!tok) {
        fprintf(stderr, "Failed to allocate JSON tokener: %s\n", strerror(errno));
        goto err;
    }
    json = json_tokener_parse_ex(tok, data, (int) len);
    if (!json) {
        fprintf(stderr, "Failed to parse input buffer: %s\n",
            json_tokener_error_desc(json_tokener_get_error(tok)));
        job->status = 400;
        goto done;
    }

    // The body must be exactly one JSON document. json-c stops at the end of
    // the first complete value and ignores whatever follows it.
    end = json_tokener_get_parse_end(tok);
    while (end < len && isspace((unsigned char) data[end])) {
        end++;
    }
    if (end != len) {
        job->status = 400;
        goto done;
    }

    if (!json_object_is_type(json, json_type_array)) {
        job->status = 400;
        goto done;
    }

    n = json_object_array_length(json);
    max = ContextMaxPoints(ctx);
    if (max > 0 && n > max) {
        fprintf(stderr, "Rejected request of %zu point(s), limit is %zu\n", n, max);
        job->status = 413;
        goto done;
    }
    if (n == 0) {
        job->reply = strdup("[]");
        if (!job->reply) {
            fprintf(stderr, "Failed to allocate reply: %s\n", strerror(errno));
            goto err;
        }
        job->reply_len = 2;
    } else {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        result = json_object_new_array();
        if (!result) {
            fprintf(stderr, "Failed to create JSON array for results: %s\n", strerror(errno));
            goto err;
        }
        ContextResetConsidered(ctx);
        for (size_t i = 0; i < n; i++) {
            // Only ever set when a pool is in use: the client closed the
            // connection, so the rest of this lookup would be for no one.
            if (__atomic_load_n(&job->cancelled, __ATOMIC_RELAXED)) {
                fprintf(stderr, "Abandoned request of %zu point(s) after %zu: client went away\n", n, i);
                job->status = 0;
                goto done;
            }
            coords = json_object_array_get_idx(json, i);
            // json-c represents JSON null as a NULL pointer, and its array
            // accessors are unchecked, so the type has to be proven first.
            if (!coords || !json_object_is_type(coords, json_type_array)
                    || json_object_array_length(coords) != 2) {
                job->status = 400;
                goto done;
            }
            double xVal, yVal;
            if (!coordValue(json_object_array_get_idx(coords, 0), &xVal)
                    || !coordValue(json_object_array_get_idx(coords, 1), &yVal)) {
                job->status = 400;
                goto done;
            }
            const char *src = NULL;
            double alt = ContextGetAltitudeFrom(ctx, xVal, yVal, &src);
            json_object *val = NULL;
            if (!isnan(alt)) {
                val = json_object_new_double(alt);
            }
            if (job->detail) {
                // {"m": value, "src": layer}, or {"m": null} with no src: a
                // point nothing covers has no source to name.
                json_object *point = json_object_new_object();
                if (!point) {
                    json_object_put(val);
                    fprintf(stderr, "Failed to create JSON object for a result: %s\n", strerror(errno));
                    goto err;
                }
                json_object_object_add(point, "m", val);
                if (val && src) {
                    json_object_object_add(point, "src", json_object_new_string(src));
                }
                val = point;
            }
            json_object_array_add(result, val);
        }
        size_t slen;
        const char *string = json_object_to_json_string_length(result, JSON_C_TO_STRING_SPACED, &slen);
        job->reply = (char *) malloc(slen + 1);
        if (!job->reply) {
            fprintf(stderr, "Failed to dump JSON string: %s\n", strerror(errno));
            goto err;
        }
        memcpy(job->reply, string, slen);
        job->reply[slen] = '\n';
        job->reply_len = slen + 1;
        gettimeofday(&end, NULL);
        time_t sec = end.tv_sec - start.tv_sec;
        time_t usec = end.tv_usec - start.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if (ContextVerbose(ctx)) {
            // The dataset count is what says the grid is doing its job. It
            // cannot be read off a response -- a grid handing back every
            // dataset for every cell answers identically, just slowly -- and
            // by the time that shows up as latency the cause is a guess.
            fprintf(stderr, "Lookup %zu point(s) in %ld.%06ld sec, %zu dataset(s) considered\n",
                n, sec, usec, ContextConsidered(ctx));
        }
    }
    job->status = 200;
    goto done;
err:
    job->status = 500;
done:
    if (result) {
        json_object_put(result);
    }
    if (json) {
        json_object_put(json);
    }
    if (tok) {
        json_tokener_free(tok);
    }
}

void ElevationFinish(struct elevation_job *job) {
    struct evhttp_request *req = job->req;
    if (job->status == 0) {
        // Abandoned. Nobody reads this, but the request still has to be
        // answered: that is what makes libevent free it.
        evhttp_send_error(req, 503, NULL);
    } else if (job->status != 200) {
        evhttp_send_error(req, job->status, NULL);
    } else {
        evbuffer *output = evbuffer_new();
        if (!output || evbuffer_add(output, job->reply, job->reply_len) != 0) {
            fprintf(stderr, "Failed to allocate output buffer: %s\n", strerror(errno));
            evhttp_send_error(req, 500, NULL);
        } else {
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", contentType);
            evhttp_send_reply(req, 200, "OK", output);
        }
        if (output) {
            evbuffer_free(output);
        }
    }
    jobFree(job);
}

void ElevationAbort(struct elevation_job *job, int status) {
    job->status = status;
    ElevationFinish(job);
}

void jobFree(struct elevation_job *job) {
    free(job->body);
    free(job->reply);
    free(job);
}

// Accepts only JSON numbers. json_object_get_double() coerces anything else to
// 0.0 without reporting an error, so the type has to be checked up front.
int coordValue(json_object *obj, double *val) {
    if (!obj) {
        return 0;
    }
    switch (json_object_get_type(obj)) {
    case json_type_int:
    case json_type_double:
        *val = json_object_get_double(obj);
        return isfinite(*val);
    default:
        return 0;
    }
}

// `?detail=1` asks for each point's layer. Absent means the bare array, so a
// caller that never heard of it gets byte-for-byte what it always got. Any
// other value is refused rather than read as "no": a caller that asked for
// detail and silently got bare numbers would misread every one of them.
//
// Only `detail` is looked at. Every other parameter is ignored, as the whole
// query always was -- which is why this does not use evhttp_parse_query_str():
// it rejects a query with any parameter lacking `=`, so `?nocache` would turn
// from ignored into a 400.
int detailRequested(struct evhttp_request *req, int *detail) {
    *detail = 0;
    const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
    const char *query = uri ? evhttp_uri_get_query(uri) : NULL;
    if (!query) {
        return 1;
    }
    static const char key[] = "detail";
    const size_t keylen = sizeof(key) - 1;
    int ok = 1;
    for (const char *p = query; ok; ) {
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= keylen && strncmp(p, key, keylen) == 0 &&
            (len == keylen || p[keylen] == '=')) {
            if (len == keylen + 2 && p[keylen + 1] == '1') {
                *detail = 1;
            } else {
                ok = 0;
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return ok;
}
