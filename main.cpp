#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>

#include <gdal.h>
#include <cpl_string.h>
#include <ogr_srs_api.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>

#include "elevation.h"
#include "context.h"

static void do_term(int sig, short events, void *arg) {
    (void) events;
    struct event_base *base = (struct event_base *) arg;
    event_base_loopbreak(base);
    fprintf(stderr, "Got signal %d, terminating...\n", sig);
}

static const char *defaultAddress = "0.0.0.0";
static const int defaultPort = 80;
static const char *defaultSRS = "WGS84";
static const char *defaultURI = "/v1/elevations";
static const char *defaultAuth = "";
static const long defaultMaxPoints = 100000;

int main(int argc, char **argv) {
    struct context *ctx = NULL;
    struct event_base *base = NULL;
    struct evhttp *http = NULL;
    struct evhttp_bound_socket *handle = NULL;
    struct event *term = NULL, *quit = NULL;
    int opt, ret = 0, port = defaultPort, verbose = 1;
    long maxPoints = defaultMaxPoints;
    const char *addr = defaultAddress;
    const char *srs = defaultSRS;
    const char *uri = defaultURI;
    const char *auth = defaultAuth;

    while ((opt = getopt(argc, argv, "a:p:u:s:A:m:q")) != -1) {
        switch (opt) {
            case 'a': addr = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'u': uri = optarg; break;
            case 's': srs = optarg; break;
            case 'A': auth = optarg; break;
            case 'm': {
                // atol() cannot report failure, and its 0 on garbage would
                // quietly mean "unlimited" -- i.e. a typo would disable both
                // the point cap and the body-size cap.
                char *rest = NULL;
                errno = 0;
                maxPoints = strtol(optarg, &rest, 10);
                // rest == optarg means no digits were consumed at all, which
                // is how an empty argument slips through: it converts to 0
                // and leaves nothing trailing to reject.
                if (errno || !rest || rest == optarg || *rest || maxPoints < 0) {
                    fprintf(stderr, "Invalid -m value: %s\n", optarg);
                    exit(1);
                }
                break;
            }
            case 'q': verbose = 0; break;
            default : fprintf(stderr, "Unknown option %c\n", opt); break;
        }
    }

    if (optind >= argc || (argc-optind) > 1 || maxPoints < 0) {
        fprintf(stdout, "Usage: %s [options] <DEM file or directory of DEM files>\n", argv[0]);
        fprintf(stdout, "Options:\n");
        fprintf(stdout, "    -a <addr> : Address to bind HTTP (default: %s)\n", defaultAddress);
        fprintf(stdout, "    -p <port> : Port to bind HTTP (default: %d)\n", defaultPort);
        fprintf(stdout, "    -u <URI>  : URI to serve REST (default: %s)\n", defaultURI);
        fprintf(stdout, "    -s <SRS>  : SRS of requested coordinates (default: %s)\n", defaultSRS);
        fprintf(stdout, "    -A <auth> : 'Authorization' header to control access, 401 status will be replied if not matched. (default: none)\n");
        fprintf(stdout, "    -m <max>  : Maximum number of points per request, 0 for unlimited (default: %ld)\n", defaultMaxPoints);
        fprintf(stdout, "    -q        : Do not log every lookup\n");
        exit(1);
    }

    // stdout is fully buffered when it is not a tty, which hides the startup
    // progress in container logs until the process exits.
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *path = argv[optind];
    GDALAllRegister();
    ctx = ContextCreate(path, srs, auth);
    if (!ctx) {
        ret = 1;
        goto err;
    }
    ContextSetMaxPoints(ctx, (size_t) maxPoints);
    ContextSetVerbose(ctx, verbose);

    if (ContextEmpty(ctx)) {
        fprintf(stderr, "No DEM found: %s\n", path);
        ret = 1;
        goto err;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "Failed to ignore SIGPIPE: %s\n", strerror(errno));
        ret = 1;
        goto err;
    }

    base = event_base_new();
    if (!base) {
        fprintf(stderr, "Failed to create event_base: %s\n", strerror(errno));
        ret = 1;
        goto err;
    }

    http = evhttp_new(base);
    if (!http) {
        fprintf(stderr, "Failed to create evhttp: %s\n", strerror(errno));
        ret = 1;
        goto err;
    }

    // libevent defaults both limits to EV_SIZE_MAX, so without this an
    // arbitrarily large POST is buffered in full before the callback runs.
    if (maxPoints > 0) {
        evhttp_set_max_body_size(http, (size_t) maxPoints * 64 + 4096);
    }
    evhttp_set_max_headers_size(http, 16384);

    evhttp_set_cb(http, uri, elevation_request_cb, ctx);

    handle = evhttp_bind_socket_with_handle(http, addr, port);
    if (!handle) {
        fprintf(stderr, "Failed to bind port %d: %s\n", port, strerror(errno));
        ret = 1;
        goto err;
    }

    term = evsignal_new(base, SIGINT, do_term, base);
    if (!term || event_add(term, NULL)) {
        fprintf(stderr, "Failed to install SIGINT handler: %s\n", strerror(errno));
        ret = 1;
        goto err;
    }

    // Container runtimes stop a service with SIGTERM, not SIGINT.
    quit = evsignal_new(base, SIGTERM, do_term, base);
    if (!quit || event_add(quit, NULL)) {
        fprintf(stderr, "Failed to install SIGTERM handler: %s\n", strerror(errno));
        ret = 1;
        goto err;
    }

    fprintf(stderr, "Serving http://%s:%d%s\n", addr, port, uri);
    ret = event_base_dispatch(base);

err:
    if (http) {
        evhttp_free(http);
    }
    if (term) {
        event_free(term);
    }
    if (quit) {
        event_free(quit);
    }
    if (base) {
        event_base_free(base);
    }
    if (ctx) {
        ContextFree(ctx);
    }
    GDALDestroyDriverManager();
    return ret;
}
