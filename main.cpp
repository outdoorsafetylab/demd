#include <stdio.h>
#ifdef ALPINE
#include <gdal.h>
#include <cpl_string.h>
#include <ogr_spatialref.h>
#else
#include <gdal/gdal.h>
#include <gdal/cpl_string.h>
#include <gdal/ogr_spatialref.h>
#endif
#include <signal.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>
#include <math.h>

#include "elevation.h"
#include "context.h"

static void do_term(int sig, short events, void *arg) {
	struct event_base *base = (struct event_base *) arg;
	event_base_loopbreak(base);
	fprintf(stderr, "Got signal %d, terminating...\n", sig);
}

static const char *defaultAddress = "0.0.0.0";
static const int defaultPort = 80;
static const char *defaultSRS = "WGS84";
static const char *defaultURI = "/v1/elevations";

int main(int argc, char **argv) {
    struct context *ctx = NULL;
    struct event_base *base = NULL;
    struct evhttp *http = NULL;
    struct evhttp_bound_socket *handle = NULL;
	struct event *term = NULL;
    int opt, ret = 0, port = defaultPort;
    const char *addr = defaultAddress;
    const char *srs = defaultSRS;
    const char *uri = defaultURI;

    while ((opt = getopt(argc, argv, "a:p:u:s:")) != -1) {
		switch (opt) {
			case 'a': addr = optarg; break;
			case 'p': port = atoi(optarg); break;
			case 'u': uri = optarg; break;
			case 's': srs = optarg; break;
			default : fprintf(stderr, "Unknown option %c\n", opt); break;
		}
	}

    if (optind >= argc || (argc-optind) > 1) {
		fprintf(stdout, "Usage: %s [options] <DEM file or directory of DEM files>\n", argv[0]);
		fprintf(stdout, "Options:\n");
		fprintf(stdout, "    -a <addr> : Address to bind HTTP (default: %s)\n", defaultAddress);
		fprintf(stdout, "    -p <port> : Port to bind HTTP (default: %d)\n", defaultPort);
		fprintf(stdout, "    -u <URI>  : URI to serve REST (default: %s)\n", defaultURI);
		fprintf(stdout, "    -s <SRS>  : SRS of requested coordinates (default: %s)\n", defaultSRS);
		exit(1);
	}

    const char *path = argv[optind];
    GDALAllRegister();
    ctx = ContextCreate(path, srs);
    if (!ctx) {
		ret = 1;
		goto err;
    }

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

    evhttp_set_cb(http, uri, elevation_request_cb, ctx);

    handle = evhttp_bind_socket_with_handle(http, addr, port);
	if (!handle) {
		fprintf(stderr, "Failed to bind port %d: %s\n", port, strerror(errno));
		ret = 1;
		goto err;
	}

    term = evsignal_new(base, SIGINT, do_term, base);
	if (!term) {
		fprintf(stderr, "Failed to create signal handler: %s\n", strerror(errno));
		ret = 1;
		goto err;
    }

	if (event_add(term, NULL)) {
		fprintf(stderr, "Failed to enable signal handler: %s\n", strerror(errno));
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
	if (base) {
		event_base_free(base);
    }
    if (ctx) {
        ContextFree(ctx);
    }
    GDALDestroyDriverManager();
	return ret;
}
