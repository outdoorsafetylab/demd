#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

#include "dataset.h"
#include "context.h"

static int endsWith(const char *str, const char *suffix);
static void joinPath(char *dst, size_t n, const char *dir, const char *file);
static int isDir(const char *path);
static int exist(const char *path);
static int isDEM(const struct dirent *ent);
static void contextAddPath(struct context *ctx, const char *path, const char *srs);
static void contextAddDataset(struct context *ctx, const char *filepath, const char *srs);

struct dataset_item {
    struct dataset *dataset;
    TAILQ_ENTRY(dataset_item) entry;
};

// A tail queue, not a list: datasets are consulted in insertion order, so the
// order the operator gave has to survive loading.
TAILQ_HEAD(dataset_list, dataset_item);

struct context {
    struct dataset_list datasets;
    size_t num_datasets;
    size_t max_points;
    int verbose;
    char *auth;
};

struct context *ContextCreate(const char **paths, size_t npaths, const char *srs, const char *auth) {
    struct context *ctx = (context *) calloc(1, sizeof(struct context));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate context: %s\n", strerror(errno));
        return NULL;
    }
    TAILQ_INIT(&ctx->datasets);
    ctx->verbose = 1;
    for (size_t i = 0; i < npaths; i++) {
        contextAddPath(ctx, paths[i], srs);
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

void contextAddPath(struct context *ctx, const char *path, const char *srs) {
    if (!exist(path)) {
        fprintf(stderr, "%s: %s\n", strerror(ENOENT), path);
        return;
    }
    if (!isDir(path)) {
        contextAddDataset(ctx, path, srs);
        return;
    }
    // scandir with alphasort rather than readdir: directory order is
    // arbitrary, and precedence between overlapping datasets must not depend
    // on it.
    struct dirent **entries = NULL;
    int n = scandir(path, &entries, isDEM, alphasort);
    if (n < 0) {
        fprintf(stderr, "Failed to scan directory %s: %s\n", path, strerror(errno));
        return;
    }
    char filepath[1024];
    for (int i = 0; i < n; i++) {
        joinPath(filepath, sizeof(filepath), path, entries[i]->d_name);
        contextAddDataset(ctx, filepath, srs);
        free(entries[i]);
    }
    free(entries);
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

double ContextGetAltitude(struct context *ctx, double x, double y) {
    struct dataset_item *item;
    double alt;
    TAILQ_FOREACH(item, &ctx->datasets, entry) {
        alt = DatasetGetAltitude(item->dataset, x, y);
        if (!isnan(alt)) {
            return alt;
        }
    }
    return NAN;
}

void contextAddDataset(struct context *ctx, const char *filepath, const char *srs) {
    struct dataset *dataset = DatasetCreate(filepath, srs);
    if (!dataset) {
        fprintf(stderr, "Failed to load dataset: %s\n", filepath);
        return;
    }
    struct dataset_item *item = (struct dataset_item *) calloc(1, sizeof(struct dataset_item));
    if (!item) {
        fprintf(stderr, "Failed to allocate dataset item: %s\n", strerror(errno));
        DatasetFree(dataset);
        return;
    }
    double top, left, bottom, right;
    DatasetGetBounds(dataset, &top, &left, &bottom, &right);
    ctx->num_datasets++;
    printf("Dataset %zu loaded: %s => (%f,%f,%f,%f)\n",
        ctx->num_datasets, filepath, top, left, bottom, right);
    item->dataset = dataset;
    TAILQ_INSERT_TAIL(&ctx->datasets, item, entry);
}

int isDEM(const struct dirent *ent) {
    return endsWith(ent->d_name, ".tif") || endsWith(ent->d_name, ".hgt");
}

int endsWith(const char *str, const char *suffix) {
    if (!str || !suffix)
        return 0;
    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr)
        return 0;
    return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

void joinPath(char *dst, size_t n, const char *dir, const char *file) {
    if (endsWith(dir, "/")) {
        snprintf(dst, n, "%s%s", dir, file);
    } else {
        snprintf(dst, n, "%s/%s", dir, file);
    }
}

int exist(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int isDir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}
