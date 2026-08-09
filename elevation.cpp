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

#include "elevation.h"

static const char *contentType = "application/json; charset=utf-8";

static int coordValue(json_object *obj, double *val);

void elevation_request_cb(struct evhttp_request *req, void *arg) {
    context *ctx = (context *)arg;
    char *data = NULL;
    json_object *coords, *json = NULL, *result = NULL;
    json_tokener *tok = NULL;
    size_t len, end, n, max;
    evbuffer *input, *output = NULL;

    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_POST:
        break;
    default:
        evhttp_send_error(req, 405, NULL);
        return;
    }

    const char *auth = ContextAuth(ctx);
    if (auth) {
        struct evkeyvalq *headers = evhttp_request_get_input_headers(req);
        const char *value = evhttp_find_header(headers, "Authorization");
        if (!value || strcmp(auth, value)) {
            evhttp_send_error(req, 401, NULL);
            return;
        }
    }

    output = evbuffer_new();
    if (!output) {
        fprintf(stderr, "Failed to allocate output buffer: %s\n", strerror(errno));
        goto err;
    }

    input = evhttp_request_get_input_buffer(req);
    if (!input) {
        fprintf(stderr, "Failed to get input buffer: %s\n", strerror(errno));
        goto err;
    }

    len = evbuffer_get_length(input);
    if (len == 0) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    }
    if (len > INT_MAX) {
        evhttp_send_error(req, 413, NULL);
        goto done;
    }

    data = (char *) malloc(len);
    if (!data) {
        fprintf(stderr, "Failed to allocate %zu bytes for input: %s\n", len, strerror(errno));
        goto err;
    }
    if (evbuffer_copyout(input, data, len) != (ev_ssize_t) len) {
        fprintf(stderr, "Failed to drain input buffer: %s\n", strerror(errno));
        goto err;
    }

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
        evhttp_send_error(req, 400, NULL);
        goto done;
    }

    // The body must be exactly one JSON document. json-c stops at the end of
    // the first complete value and ignores whatever follows it.
    end = json_tokener_get_parse_end(tok);
    while (end < len && isspace((unsigned char) data[end])) {
        end++;
    }
    if (end != len) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    }

    if (!json_object_is_type(json, json_type_array)) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    }

    n = json_object_array_length(json);
    max = ContextMaxPoints(ctx);
    if (max > 0 && n > max) {
        fprintf(stderr, "Rejected request of %zu point(s), limit is %zu\n", n, max);
        evhttp_send_error(req, 413, NULL);
        goto done;
    }
    if (n == 0) {
        evbuffer_add(output, "[]", 2);
    } else {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        result = json_object_new_array();
        if (!result) {
            fprintf(stderr, "Failed to create JSON array for results: %s\n", strerror(errno));
            goto err;
        }
        for (size_t i = 0; i < n; i++) {
            coords = json_object_array_get_idx(json, i);
            // json-c represents JSON null as a NULL pointer, and its array
            // accessors are unchecked, so the type has to be proven first.
            if (!coords || !json_object_is_type(coords, json_type_array)
                    || json_object_array_length(coords) != 2) {
                evhttp_send_error(req, 400, NULL);
                goto done;
            }
            double xVal, yVal;
            if (!coordValue(json_object_array_get_idx(coords, 0), &xVal)
                    || !coordValue(json_object_array_get_idx(coords, 1), &yVal)) {
                evhttp_send_error(req, 400, NULL);
                goto done;
            }
            double alt = ContextGetAltitude(ctx, xVal, yVal);
            json_object *val = NULL;
            if (!isnan(alt)) {
                val = json_object_new_double(alt);
            }
            json_object_array_add(result, val);
        }
        const char *string = json_object_to_json_string(result);
        if (evbuffer_add(output, string, strlen(string)) != 0
                || evbuffer_add(output, "\n", 1) != 0) {
            fprintf(stderr, "Failed to dump JSON string: %s\n", strerror(errno));
            goto err;
        }
        gettimeofday(&end, NULL);
        time_t sec = end.tv_sec - start.tv_sec;
        time_t usec = end.tv_usec - start.tv_usec;
        if (usec < 0) {
            usec += 1000000;
            sec--;
        }
        if (ContextVerbose(ctx)) {
            fprintf(stderr, "Lookup %zu point(s) in %ld.%06ld sec\n", n, sec, usec);
        }
    }
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", contentType);
    evhttp_send_reply(req, 200, "OK", output);
    goto done;
err:
    evhttp_send_error(req, 500, NULL);
done:
    if (output) {
        evbuffer_free(output);
    }
    if (result) {
        json_object_put(result);
    }
    if (json) {
        json_object_put(json);
    }
    if (tok) {
        json_tokener_free(tok);
    }
    if (data) {
        free(data);
    }
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
