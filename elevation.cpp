#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <math.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>

#include "context.h"

#include "elevation.h"

static const char *contentType = "application/json; charset=utf-8";

void elevation_request_cb(struct evhttp_request *req, void *arg) {
    context *ctx = (context *)arg;
    char *data = NULL;
    json_object *coords, *json = NULL, *result = NULL;
    size_t len;
    int n;
    evbuffer *input, *output = NULL;

    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_POST:
        break;
    default:
        evhttp_send_error(req, 405, NULL);
        return;
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
    if (len <= 0) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    }

    data = (char *) malloc(len);
    if (evbuffer_copyout(input, data, len) != len) {
        fprintf(stderr, "Failed to drain input buffer: %s\n", strerror(errno));
        goto err;
    }

    json_tokener_error err;
    json = json_tokener_parse_verbose(data, &err);
    if (!json) {
        fprintf(stderr, "Failed to parse input buffer: %s\n", json_tokener_error_desc(err));
        goto err;
    }
    
    if (!json_object_is_type(json, json_type_array)) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    }

    n = json_object_array_length(json);
    if (n < 0) {
        evhttp_send_error(req, 400, NULL);
        goto done;
    } else if (n == 0) {
        evbuffer_add(output, "[]", 2);
    } else {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        result = json_object_new_array();
        if (!result) {
            fprintf(stderr, "Failed to create JSON array for results: %s\n", strerror(errno));
            goto err;
        }
        for (int i = 0; i < n; i++) {
            coords = json_object_array_get_idx(json, i);
            if (json_object_array_length(coords) != 2) {
                evhttp_send_error(req, 400, NULL);
                goto done;
            }
            json_object *x = json_object_array_get_idx(coords, 0);
            json_object *y = json_object_array_get_idx(coords, 1);
            double xVal = json_object_get_double(x);
            if (errno == EINVAL) {
                evhttp_send_error(req, 400, NULL);
                goto done;
            }
            double yVal = json_object_get_double(y);
            if (errno == EINVAL) {
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
        fprintf(stderr, "Lookup %d point(s) in %ld.%06ld sec\n", n, sec, usec);
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
    if (data) {
        free(data);
    }
}
