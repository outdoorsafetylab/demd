#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <ogr_srs_api.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>
#include <json-c/json.h>

#include "elevation.h"
#include "context.h"
#include "paths.h"
#include "index.h"
#include "srs.h"

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
static const long defaultMaxOpen = 500;

static long parseCount(const char *arg, const char *flag, long ceiling);
static int collectSources(int fromStdin, char **argv, int argc, char ***out, size_t *outn);
static void freeSources(char **list, size_t n);
static void enableHTTPRetry(void);
static void usage(const char *argv0);

int main(int argc, char **argv) {
    struct context *ctx = NULL;
    struct event_base *base = NULL;
    struct evhttp *http = NULL;
    struct evhttp_bound_socket *handle = NULL;
    struct event *term = NULL, *quit = NULL;
    int opt, ret = 0, port = defaultPort, verbose = 1;
    long maxPoints = defaultMaxPoints;
    long maxOpen = defaultMaxOpen;
    int fromStdin = 0;
    const char **paths = NULL;
    size_t npaths = 0;
    const char *addr = defaultAddress;
    const char *srs = defaultSRS;
    const char *uri = defaultURI;
    const char *auth = defaultAuth;
    const char *writeIndex = NULL;
    const char *verifyIndex = NULL;
    const char *printPath = NULL;

    static struct option longopts[] = {
        {"from-stdin", no_argument, NULL, 1},
        {NULL, 0, NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "a:p:u:s:A:m:n:w:W:P:q", longopts, NULL)) != -1) {
        switch (opt) {
            case 'a': addr = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'u': uri = optarg; break;
            case 's': srs = optarg; break;
            case 'A': auth = optarg; break;
            case 'w': writeIndex = optarg; break;
            case 'W': verifyIndex = optarg; break;
            case 'P': printPath = optarg; break;
            case 1:   fromStdin = 1; break;
            case 'm':
                // The body cap is derived from this below; a value that
                // overflows that arithmetic would wrap to a tiny limit rather
                // than the enormous one asked for.
                maxPoints = parseCount(optarg, "-m", (long) ((SIZE_MAX - 4096) / 64));
                break;
            case 'n':
                maxOpen = parseCount(optarg, "-n", 1000000);
                break;
            case 'q': verbose = 0; break;
            default : fprintf(stderr, "Unknown option %c\n", opt); break;
        }
    }

    // stdout is fully buffered when it is not a tty, which hides the startup
    // progress in container logs until the process exits.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Answers "why will this path not load" without starting anything, and is
    // the only way to check the gs:// and s3:// rewrites without a bucket to
    // reach: -w normalizes and then immediately opens, so those two can never
    // be exercised on their own.
    if (printPath) {
        char *vsi = PathToVSI(printPath);
        if (!vsi) {
            fprintf(stderr, "Failed to normalize path: %s\n", strerror(errno));
            return 1;
        }
        printf("%s\n", vsi);
        free(vsi);
        return 0;
    }

    if (writeIndex && verifyIndex) {
        fprintf(stderr, "-w and -W are separate modes; pass one\n");
        return 1;
    }
    if (!fromStdin && optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    enableHTTPRetry();
    GDALAllRegister();

    if (writeIndex || verifyIndex) {
        char *srsWkt = SRSSanitize(srs);
        if (!srsWkt) {
            fprintf(stderr, "Failed to sanitize SRS '%s': %s\n", srs, CPLGetLastErrorMsg());
            GDALDestroyDriverManager();
            return 1;
        }
        OGRSpatialReferenceH hReqSRS = OSRNewSpatialReference(srsWkt);
        char **sources = NULL;
        size_t nsources = 0;
        if (!hReqSRS) {
            fprintf(stderr, "Failed to create source SRS: %s\n", CPLGetLastErrorMsg());
            ret = 1;
        } else {
            SRSUseTraditionalAxisOrder(hReqSRS);
            if (!collectSources(fromStdin, argv + optind, argc - optind, &sources, &nsources)) {
                ret = 1;
            } else if (nsources == 0) {
                fprintf(stderr, "No DEM found to index\n");
                ret = 1;
            } else if (writeIndex) {
                ret = IndexGenerate(writeIndex, hReqSRS, srsWkt, sources, nsources);
            } else {
                ret = IndexVerify(verifyIndex, hReqSRS, sources, nsources);
            }
        }
        freeSources(sources, nsources);
        if (hReqSRS) {
            OSRDestroySpatialReference(hReqSRS);
        }
        CPLFree(srsWkt);
        GDALDestroyDriverManager();
        return ret;
    }

    paths = (const char **) &argv[optind];
    npaths = (size_t) (argc - optind);
    ctx = ContextCreate(paths, npaths, srs, auth, (size_t) maxOpen);
    if (!ctx) {
        ret = 1;
        goto err;
    }
    ContextSetMaxPoints(ctx, (size_t) maxPoints);
    ContextSetVerbose(ctx, verbose);

    if (ContextEmpty(ctx)) {
        fprintf(stderr, "No DEM found in %zu path(s), first is: %s\n", npaths, paths[0]);
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

// atol() cannot report failure, and its 0 on garbage would quietly mean
// "unlimited" -- i.e. a typo would disable the cap it was meant to set.
long parseCount(const char *arg, const char *flag, long ceiling) {
    char *rest = NULL;
    errno = 0;
    long value = strtol(arg, &rest, 10);
    // rest == arg means no digits were consumed at all, which is how an empty
    // argument slips through: it converts to 0 and leaves nothing trailing to
    // reject.
    if (errno || !rest || rest == arg || *rest || value < 0) {
        fprintf(stderr, "Invalid %s value: %s\n", flag, arg);
        exit(1);
    }
    if (value > ceiling) {
        fprintf(stderr, "Too large %s value: %s\n", flag, arg);
        exit(1);
    }
    return value;
}

// Builds the ordered source list the index is generated from or checked
// against. Directories go through the same listing the server uses, so the
// index cannot disagree with it about order.
int collectSources(int fromStdin, char **argv, int argc, char ***out, size_t *outn) {
    char **list = NULL;
    size_t n = 0, capacity = 0;

    #define PUSH(str) do { \
        if (n == capacity) { \
            size_t grown = capacity ? capacity * 2 : 64; \
            char **bigger = (char **) realloc(list, grown * sizeof(char *)); \
            if (!bigger) { \
                fprintf(stderr, "Failed to grow source list: %s\n", strerror(errno)); \
                free(str); \
                freeSources(list, n); \
                return 0; \
            } \
            list = bigger; \
            capacity = grown; \
        } \
        list[n++] = (str); \
    } while (0)

    if (fromStdin) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, stdin)) != -1) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (len == 0) {
                continue;
            }
            // `gsutil ls` and `aws s3 ls` emit gs:// and s3://, which GDAL
            // rejects outright. Normalizing here, once, means nothing
            // downstream -- the index, the server -- has to know any scheme.
            char *vsi = PathToVSI(line);
            if (!vsi) {
                fprintf(stderr, "Failed to normalize path: %s\n", strerror(errno));
                free(line);
                freeSources(list, n);
                return 0;
            }
            PUSH(vsi);
        }
        free(line);
    }
    for (int i = 0; i < argc; i++) {
        if (PathIsDir(argv[i])) {
            char **dems = NULL;
            int count = PathListDEMs(argv[i], &dems);
            if (count < 0) {
                freeSources(list, n);
                return 0;
            }
            for (int j = 0; j < count; j++) {
                char *vsi = PathToVSI(dems[j]);
                if (!vsi) {
                    fprintf(stderr, "Failed to normalize path: %s\n", strerror(errno));
                    PathListFree(dems, (size_t) count);
                    freeSources(list, n);
                    return 0;
                }
                PUSH(vsi);
            }
            PathListFree(dems, (size_t) count);
        } else {
            char *vsi = PathToVSI(argv[i]);
            if (!vsi) {
                fprintf(stderr, "Failed to normalize path: %s\n", strerror(errno));
                freeSources(list, n);
                return 0;
            }
            PUSH(vsi);
        }
    }
    #undef PUSH

    *out = list;
    *outn = n;
    return 1;
}

void freeSources(char **list, size_t n) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

// GDAL does not retry HTTP by default (GDAL_HTTP_MAX_RETRY is 0), so a single
// 429 or 503 from object storage arrives here as a hard open failure. Set only
// when the operator has not: CPLSetConfigOption takes precedence over the
// environment, so writing unconditionally would override a deliberate choice.
void enableHTTPRetry(void) {
    if (!CPLGetConfigOption("GDAL_HTTP_MAX_RETRY", NULL)) {
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "3");
    }
    if (!CPLGetConfigOption("GDAL_HTTP_RETRY_DELAY", NULL)) {
        CPLSetConfigOption("GDAL_HTTP_RETRY_DELAY", "1");
    }
}

void usage(const char *argv0) {
    fprintf(stdout, "Usage: %s [options] <DEM file, directory or index>...\n", argv0);
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "    -a <addr> : Address to bind HTTP (default: %s)\n", defaultAddress);
    fprintf(stdout, "    -p <port> : Port to bind HTTP (default: %d)\n", defaultPort);
    fprintf(stdout, "    -u <URI>  : URI to serve REST (default: %s)\n", defaultURI);
    fprintf(stdout, "    -s <SRS>  : SRS of requested coordinates (default: %s)\n", defaultSRS);
    fprintf(stdout, "    -A <auth> : 'Authorization' header to control access, 401 status will be replied if not matched. (default: none)\n");
    fprintf(stdout, "    -m <max>  : Maximum number of points per request, 0 for unlimited (default: %ld)\n", defaultMaxPoints);
    fprintf(stdout, "    -n <max>  : Maximum DEM files kept open at once, 0 for unlimited (default: %ld)\n", defaultMaxOpen);
    fprintf(stdout, "    -q        : Do not log every lookup\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Index modes (build time, not serving):\n");
    fprintf(stdout, "    -w <index>    : Write an index of the given sources and exit\n");
    fprintf(stdout, "    -W <index>    : Check an index against the given sources and exit\n");
    fprintf(stdout, "    -P <path>     : Print the /vsi* form of a path and exit\n");
    fprintf(stdout, "    --from-stdin  : Take the source paths from stdin, one per line\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Paths are searched in the order given, and files within a directory in\n");
    fprintf(stdout, "sorted order. The first dataset holding a value for a coordinate wins,\n");
    fprintf(stdout, "so put higher-priority data first.\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "A directory holding a `%s` file, or an index passed directly, loads\n", "demd.index");
    fprintf(stdout, "from that index and opens each DEM only when a lookup falls inside it.\n");
}
