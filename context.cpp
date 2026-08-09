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
static void contextAddDataset(struct context *ctx, const char *filepath, const char *srs);

struct dataset_item {
    struct dataset *dataset;
    LIST_ENTRY(dataset_item) entry;
};

LIST_HEAD(dataset_list, dataset_item);

struct context {
    struct dataset_list datasets;
    size_t num_datasets;
    size_t max_points;
    int verbose;
    char *auth;
};

struct context *ContextCreate(const char *path, const char *srs, const char *auth) {
    struct context *ctx = (context *) calloc(1, sizeof(struct context));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate context: %s\n", strerror(errno));
        return NULL;
    }
    LIST_INIT(&ctx->datasets);
    ctx->verbose = 1;
    DIR *d;
    struct dirent *ent;
    char filepath[1024];
    if (exist(path)) {
        if (isDir(path)) {
            d = opendir(path);
            if (d) {
                while ((ent = readdir(d)) != NULL) {
                    if (endsWith(ent->d_name, ".tif") || endsWith(ent->d_name, ".hgt")) {
                        joinPath(filepath, sizeof(filepath), path, ent->d_name);
                        contextAddDataset(ctx, filepath, srs);
                    }
                }
                closedir(d);
            } else {
                fprintf(stderr, "Failed to open directory %s: %s\n", path, strerror(errno));
            }
        } else {
            contextAddDataset(ctx, path, srs);
        }
    } else {
        fprintf(stderr, "%s: %s\n", strerror(ENOENT), path);
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

void ContextFree(struct context *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->auth) {
        free(ctx->auth);
    }
    // Cannot use LIST_FOREACH here: it advances through the entry that the
    // body just freed.
    struct dataset_item *item = LIST_FIRST(&ctx->datasets);
    while (item) {
        struct dataset_item *next = LIST_NEXT(item, entry);
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
    return LIST_EMPTY(&ctx->datasets);
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
    LIST_FOREACH(item, &ctx->datasets, entry) {
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
    printf("Dataset loaded: %s => (%f,%f,%f,%f)\n", filepath, top, left, bottom, right);
    item->dataset = dataset;
    LIST_INSERT_HEAD(&ctx->datasets, item, entry);
    ctx->num_datasets++;
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
