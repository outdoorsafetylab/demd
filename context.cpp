#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_srs_api.h>

#include "srs.h"
#include "paths.h"
#include "dataset.h"
#include "index.h"
#include "grid.h"
#include "context.h"

// The conventional name looked for inside a directory. It deliberately does
// not end in .tif or .hgt, so a directory scan never mistakes it for data.
#define CONTEXT_INDEX_NAME "demd.index"

struct dataset_item {
    struct dataset *dataset;
    // The layer this dataset came from: owned by the context, shared by every
    // dataset loaded from the same path argument.
    const char *src;
    TAILQ_ENTRY(dataset_item) entry;
    TAILQ_ENTRY(dataset_item) open_entry;
    int listed;
};

// A tail queue, not a list: datasets are consulted in insertion order, so the
// order the operator gave has to survive loading.
TAILQ_HEAD(dataset_list, dataset_item);

struct context {
    struct dataset_list datasets;
    // Most recently used at the head. Open files are a bounded resource and
    // the whole point of an index is to make the dataset count unbounded.
    struct dataset_list open_datasets;
    size_t num_datasets;
    size_t num_open;
    size_t max_open;
    size_t max_points;
    size_t considered;
    int verbose;
    char *auth;
    char *srs_wkt;
    // One name per path argument, in argument order. A layer is what the
    // operator named on the command line, however many datasets it expands to.
    char **layers;
    size_t num_layers;
    OGRSpatialReferenceH hReqSRS;
    // Built once, after every path is loaded. `by_index` gives the grid's
    // answers somewhere to point: the datasets themselves live in a queue
    // because their order is precedence, and a queue cannot be indexed.
    struct dataset_grid *grid;
    struct dataset_item **by_index;
};

static void contextAddPath(struct context *ctx, const char *path);
static void contextAddDataset(struct context *ctx, const char *filepath, const char *src);
static int contextAddIndex(struct context *ctx, const char *indexPath, const char *src);
static struct dataset_item *contextAppend(struct context *ctx, struct dataset *dataset,
                                          const char *src);
static const char *contextAddLayer(struct context *ctx, const char *path);
static void contextNoteOpened(struct context *ctx, struct dataset_item *item);
static void contextTouch(struct context *ctx, struct dataset_item *item);
static int contextBuildGrid(struct context *ctx);
static double contextConsult(struct context *ctx, struct dataset_item *item, double x, double y);

struct context *ContextCreate(const char **paths, size_t npaths, const char *srs,
                              const char *auth, size_t max_open) {
    struct context *ctx = (context *) calloc(1, sizeof(struct context));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate context: %s\n", strerror(errno));
        return NULL;
    }
    TAILQ_INIT(&ctx->datasets);
    TAILQ_INIT(&ctx->open_datasets);
    ctx->verbose = 1;
    ctx->max_open = max_open;

    // The request SRS is resolved once, here, rather than per dataset. Two
    // reasons, and the first is the one that matters: with the open deferred,
    // nothing else would touch GDAL at startup, so a bad -s would surface on
    // the first query instead of exiting. The second is that a WKT string and
    // an OGRSpatialReference per tile is real memory once tiles are counted in
    // tens of thousands.
    ctx->srs_wkt = SRSSanitize(srs);
    if (!ctx->srs_wkt) {
        fprintf(stderr, "Failed to sanitize SRS '%s': %s\n", srs, CPLGetLastErrorMsg());
        ContextFree(ctx);
        return NULL;
    }
    ctx->hReqSRS = OSRNewSpatialReference(ctx->srs_wkt);
    if (!ctx->hReqSRS) {
        fprintf(stderr, "Failed to create source SRS: %s\n", CPLGetLastErrorMsg());
        ContextFree(ctx);
        return NULL;
    }
    SRSUseTraditionalAxisOrder(ctx->hReqSRS);

    for (size_t i = 0; i < npaths; i++) {
        contextAddPath(ctx, paths[i]);
    }
    if (!contextBuildGrid(ctx)) {
        ContextFree(ctx);
        return NULL;
    }
    if (strlen(auth) > 0) {
        ctx->auth = strdup(auth);
        if (!ctx->auth) {
            fprintf(stderr, "Failed to allocate auth: %s\n", strerror(errno));
            ContextFree(ctx);
            return NULL;
        }
    }
    return ctx;
}

void contextAddPath(struct context *ctx, const char *path) {
    if (!PathExists(path)) {
        fprintf(stderr, "%s: %s\n", strerror(ENOENT), path);
        return;
    }
    const char *src = contextAddLayer(ctx, path);
    if (!src) {
        return;
    }
    if (!PathIsDir(path)) {
        // Recognition is by content: an index handed over directly is still an
        // index, whatever it is called.
        if (IndexLooksLikeIndex(path)) {
            contextAddIndex(ctx, path, src);
        } else {
            contextAddDataset(ctx, path, src);
        }
        return;
    }
    char indexPath[1024];
    PathJoin(indexPath, sizeof(indexPath), path, CONTEXT_INDEX_NAME);
    if (PathExists(indexPath) && IndexLooksLikeIndex(indexPath)) {
        contextAddIndex(ctx, indexPath, src);
        return;
    }
    char **list = NULL;
    int n = PathListDEMs(path, &list);
    if (n < 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        contextAddDataset(ctx, list[i], src);
    }
    PathListFree(list, (size_t) n);
}

// Names the layer a path argument contributes: the last path component, with
// the extension dropped when the argument is a file. A directory `/data/moi2025`
// and an index file `/data/glo30.index` become `moi2025` and `glo30`.
//
// The name comes from the argument rather than from each dataset's own path on
// purpose. Entries of one layer need not share a directory -- a global layer's
// entries point into someone else's bucket, one directory per tile -- so a name
// derived per entry would not be one name per layer, and the whole point of the
// name is that consumers can look it up.
const char *contextAddLayer(struct context *ctx, const char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    size_t start = len;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    size_t end = len;
    if (!PathIsDir(path)) {
        for (size_t i = len; i > start + 1; i--) {
            if (path[i - 1] == '.') {
                end = i - 1;
                break;
            }
        }
    }
    char *name = strndup(path + start, end - start);
    char **grown = name ? (char **) realloc(ctx->layers, (ctx->num_layers + 1) * sizeof(char *)) : NULL;
    if (!grown) {
        fprintf(stderr, "Failed to allocate layer name: %s\n", strerror(errno));
        free(name);
        return NULL;
    }
    ctx->layers = grown;
    ctx->layers[ctx->num_layers++] = name;
    return name;
}

int contextAddIndex(struct context *ctx, const char *indexPath, const char *src) {
    struct dem_index *idx = IndexRead(indexPath);
    if (!idx) {
        return 0;
    }
    // The bounds in an index are expressed in the SRS it was generated with,
    // and -s is a run-time flag. Re-projecting the recorded box would not
    // recover the right answer either -- projecting a rectangle's corners is
    // not the same as projecting the raster's -- so a mismatch is refused
    // rather than papered over. OSRIsSame() so that WGS84 and EPSG:4326 and an
    // equivalent WKT are all accepted for one another.
    OGRSpatialReferenceH hIndexSRS = OSRNewSpatialReference(idx->srs_wkt);
    if (!hIndexSRS) {
        fprintf(stderr, "%s: unreadable #srs header\n", indexPath);
        IndexFree(idx);
        return 0;
    }
    SRSUseTraditionalAxisOrder(hIndexSRS);
    int same = OSRIsSame(hIndexSRS, ctx->hReqSRS);
    OSRDestroySpatialReference(hIndexSRS);
    if (!same) {
        fprintf(stderr, "%s was generated for a different SRS than the one requested; "
            "regenerate it with the same -s\n", indexPath);
        IndexFree(idx);
        return 0;
    }

    for (size_t i = 0; i < idx->n; i++) {
        struct index_entry *e = &idx->entries[i];
        struct dataset *dataset = DatasetCreateIndexed(e->path, ctx->hReqSRS,
            e->top, e->left, e->bottom, e->right);
        if (!dataset) {
            IndexFree(idx);
            return 0;
        }
        if (!contextAppend(ctx, dataset, src)) {
            DatasetFree(dataset);
            IndexFree(idx);
            return 0;
        }
    }
    // One line, not one per dataset: an index may hold tens of thousands of
    // entries, and the per-dataset line reported a successful open, which is
    // exactly what has not happened here.
    printf("Index %s: %zu dataset(s), generated %s\n",
        indexPath, idx->n, idx->generated ? idx->generated : "(unknown)");
    IndexFree(idx);
    return 1;
}

void ContextFree(struct context *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->auth) {
        free(ctx->auth);
    }
    struct dataset_item *item = TAILQ_FIRST(&ctx->datasets);
    while (item) {
        // The iteration step reads through the entry the body is about to
        // free, so the successor has to be taken first.
        struct dataset_item *next = TAILQ_NEXT(item, entry);
        if (item->dataset) {
            DatasetFree(item->dataset);
        }
        free(item);
        item = next;
    }
    GridFree(ctx->grid);
    free(ctx->by_index);
    for (size_t i = 0; i < ctx->num_layers; i++) {
        free(ctx->layers[i]);
    }
    free(ctx->layers);
    if (ctx->hReqSRS) {
        OSRDestroySpatialReference(ctx->hReqSRS);
    }
    if (ctx->srs_wkt) {
        // Allocated by OSRExportToWkt(), so it belongs to CPL's allocator.
        CPLFree(ctx->srs_wkt);
    }
    free(ctx);
}

const char *ContextAuth(struct context *ctx) {
    return ctx->auth;
}

int ContextEmpty(struct context *ctx) {
    return TAILQ_EMPTY(&ctx->datasets);
}

void ContextSetMaxPoints(struct context *ctx, size_t max) {
    ctx->max_points = max;
}

size_t ContextMaxPoints(struct context *ctx) {
    return ctx->max_points;
}

void ContextSetVerbose(struct context *ctx, int verbose) {
    ctx->verbose = verbose;
}

int ContextVerbose(struct context *ctx) {
    return ctx->verbose;
}

size_t ContextConsidered(struct context *ctx) {
    return ctx->considered;
}

void ContextResetConsidered(struct context *ctx) {
    ctx->considered = 0;
}

double ContextGetAltitude(struct context *ctx, double x, double y) {
    return ContextGetAltitudeFrom(ctx, x, y, NULL);
}

double ContextGetAltitudeFrom(struct context *ctx, double x, double y, const char **src) {
    if (src) {
        *src = NULL;
    }
    // Only an empty context has no grid, and main() refuses to serve one.
    if (!ctx->grid) {
        return NAN;
    }
    struct grid_cursor cur;
    size_t index;
    GridBegin(ctx->grid, x, y, &cur);
    while (GridNext(&cur, &index)) {
        ctx->considered++;
        struct dataset_item *item = ctx->by_index[index];
        double alt = contextConsult(ctx, item, x, y);
        if (!isnan(alt)) {
            if (src) {
                *src = item->src;
            }
            return alt;
        }
    }
    return NAN;
}

// One dataset's turn. The grid narrows which datasets are asked; it never
// decides the answer, so the bounds test stays here as the authority -- a cell
// holds every box that overlaps it, not only those containing the point.
double contextConsult(struct context *ctx, struct dataset_item *item, double x, double y) {
    if (!DatasetContains(item->dataset, x, y)) {
        return NAN;
    }
    if (!DatasetIsOpen(item->dataset)) {
        if (!DatasetOpen(item->dataset)) {
            // Either it is broken or its backoff has not expired. Falling
            // through is what the next dataset in precedence order is for.
            return NAN;
        }
        contextNoteOpened(ctx, item);
    } else if (ctx->max_open > 0) {
        contextTouch(ctx, item);
    }
    return DatasetGetAltitude(item->dataset, x, y);
}

// Fails the startup rather than falling back to a walk over every dataset.
// That walk would be correct, but nothing would ever execute it: it is
// unreachable except on allocation failure, so its first run would be inside
// the memory exhaustion that caused it. A container restarting beats a second
// lookup path nobody has tested.
int contextBuildGrid(struct context *ctx) {
    if (ctx->num_datasets == 0) {
        return 1;
    }
    struct grid_box *boxes = (struct grid_box *) calloc(ctx->num_datasets, sizeof(struct grid_box));
    ctx->by_index = (struct dataset_item **) calloc(ctx->num_datasets, sizeof(struct dataset_item *));
    if (!boxes || !ctx->by_index) {
        fprintf(stderr, "Failed to allocate lookup grid: %s\n", strerror(errno));
        free(boxes);
        free(ctx->by_index);
        ctx->by_index = NULL;
        return 0;
    }
    struct dataset_item *item;
    size_t i = 0;
    TAILQ_FOREACH(item, &ctx->datasets, entry) {
        DatasetGetBounds(item->dataset, &boxes[i].top, &boxes[i].left,
            &boxes[i].bottom, &boxes[i].right);
        ctx->by_index[i] = item;
        i++;
    }
    ctx->grid = GridBuild(boxes, ctx->num_datasets);
    free(boxes);
    if (!ctx->grid) {
        free(ctx->by_index);
        ctx->by_index = NULL;
        return 0;
    }
    size_t nx, ny, oversized;
    GridStats(ctx->grid, &nx, &ny, &oversized);
    printf("Lookup grid: %zux%zu cells, %zu oversized dataset(s)\n", nx, ny, oversized);
    return 1;
}

void contextAddDataset(struct context *ctx, const char *filepath, const char *src) {
    struct dataset *dataset = DatasetCreate(filepath, ctx->hReqSRS);
    if (!dataset) {
        fprintf(stderr, "Failed to load dataset: %s\n", filepath);
        return;
    }
    struct dataset_item *item = contextAppend(ctx, dataset, src);
    if (!item) {
        DatasetFree(dataset);
        return;
    }
    // Opened eagerly, so it takes a slot like any other open dataset.
    contextNoteOpened(ctx, item);
    double top, left, bottom, right;
    DatasetGetBounds(dataset, &top, &left, &bottom, &right);
    printf("Dataset %zu loaded: %s => (%f,%f,%f,%f)\n",
        ctx->num_datasets, filepath, top, left, bottom, right);
}

struct dataset_item *contextAppend(struct context *ctx, struct dataset *dataset,
                                   const char *src) {
    struct dataset_item *item = (struct dataset_item *) calloc(1, sizeof(struct dataset_item));
    if (!item) {
        fprintf(stderr, "Failed to allocate dataset item: %s\n", strerror(errno));
        return NULL;
    }
    item->dataset = dataset;
    item->src = src;
    ctx->num_datasets++;
    TAILQ_INSERT_TAIL(&ctx->datasets, item, entry);
    return item;
}

void contextNoteOpened(struct context *ctx, struct dataset_item *item) {
    TAILQ_INSERT_HEAD(&ctx->open_datasets, item, open_entry);
    item->listed = 1;
    ctx->num_open++;
    while (ctx->max_open > 0 && ctx->num_open > ctx->max_open) {
        struct dataset_item *victim = TAILQ_LAST(&ctx->open_datasets, dataset_list);
        if (!victim || victim == item) {
            break;
        }
        TAILQ_REMOVE(&ctx->open_datasets, victim, open_entry);
        victim->listed = 0;
        ctx->num_open--;
        DatasetClose(victim->dataset);
    }
}

void contextTouch(struct context *ctx, struct dataset_item *item) {
    if (!item->listed) {
        return;
    }
    if (TAILQ_FIRST(&ctx->open_datasets) == item) {
        return;
    }
    TAILQ_REMOVE(&ctx->open_datasets, item, open_entry);
    TAILQ_INSERT_HEAD(&ctx->open_datasets, item, open_entry);
}
