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
};

context *ContextCreate(const char *path, const char *srs) {
    struct context *ctx = (context *) calloc(sizeof(struct context), 1);
    LIST_INIT(&ctx->datasets);
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
            }
        } else {
            contextAddDataset(ctx, path, srs);
        }
    } else {
        printf("%s: %s\n", strerror(ENOENT), path);
    }
    return ctx;
}

void ContextFree(context *ctx) {
    if (!ctx) {
        return;
    }
    struct dataset_item *item;
    LIST_FOREACH(item, &ctx->datasets, entry) {
        if (item->dataset) {
            DatasetFree(item->dataset);
        }
        free(item);
    }
    free(ctx);
}

void contextAddDataset(struct context *ctx, const char *filepath, const char *srs) {
    struct dataset *dataset = DatasetCreate(filepath, srs);
    if (dataset) {
        double top, left, bottom, right;
        DatasetGetBounds(dataset, &top, &left, &bottom, &right);
        printf("Dataset loaded: %s => (%f,%f,%f,%f)\n", filepath, top, left, bottom, right);
        struct dataset_item *item = (struct dataset_item *) calloc(sizeof(struct dataset_item), 1);
        item->dataset = dataset;
        LIST_INSERT_HEAD(&ctx->datasets, item, entry);
        ctx->num_datasets++;
    } else {
        printf("Failed to load dataset: %s => %s\n", filepath, strerror(errno));
        return;
    }
}

int ContextEmpty(context *ctx) {
    return LIST_EMPTY(&ctx->datasets);
}

double ContextGetAltitude(context *ctx, double x, double y) {
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
    stat(path, &st);
    return S_ISDIR(st.st_mode);
}
