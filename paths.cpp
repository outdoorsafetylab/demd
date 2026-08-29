#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "paths.h"

static int endsWith(const char *str, const char *suffix);
static int isDEMEntry(const struct dirent *ent);
static char *concat(const char *prefix, const char *rest);

int PathExists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int PathIsDir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

void PathJoin(char *dst, size_t n, const char *dir, const char *file) {
    if (endsWith(dir, "/")) {
        snprintf(dst, n, "%s%s", dir, file);
    } else {
        snprintf(dst, n, "%s/%s", dir, file);
    }
}

int PathIsDEM(const char *name) {
    return endsWith(name, ".tif") || endsWith(name, ".hgt");
}

int PathListDEMs(const char *dir, char ***out) {
    // scandir with alphasort rather than readdir: directory order is
    // arbitrary, and precedence between overlapping datasets must not depend
    // on it.
    struct dirent **entries = NULL;
    int n = scandir(dir, &entries, isDEMEntry, alphasort);
    if (n < 0) {
        fprintf(stderr, "Failed to scan directory %s: %s\n", dir, strerror(errno));
        return -1;
    }
    char **list = (char **) calloc((size_t) n + 1, sizeof(char *));
    if (!list) {
        fprintf(stderr, "Failed to allocate path list: %s\n", strerror(errno));
        for (int i = 0; i < n; i++) {
            free(entries[i]);
        }
        free(entries);
        return -1;
    }
    int kept = 0;
    for (int i = 0; i < n; i++) {
        char filepath[1024];
        PathJoin(filepath, sizeof(filepath), dir, entries[i]->d_name);
        list[kept] = strdup(filepath);
        if (!list[kept]) {
            fprintf(stderr, "Failed to copy path: %s\n", strerror(errno));
            for (int j = i; j < n; j++) {
                free(entries[j]);
            }
            free(entries);
            PathListFree(list, (size_t) kept);
            return -1;
        }
        kept++;
        free(entries[i]);
    }
    free(entries);
    *out = list;
    return kept;
}

void PathListFree(char **list, size_t n) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

char *PathToVSI(const char *path) {
    if (strncmp(path, "gs://", 5) == 0) {
        return concat("/vsigs/", path + 5);
    }
    if (strncmp(path, "s3://", 5) == 0) {
        return concat("/vsis3/", path + 5);
    }
    // The whole URL, scheme included, is what /vsicurl takes.
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        return concat("/vsicurl/", path);
    }
    return strdup(path);
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

int isDEMEntry(const struct dirent *ent) {
    return PathIsDEM(ent->d_name);
}

char *concat(const char *prefix, const char *rest) {
    size_t n = strlen(prefix) + strlen(rest) + 1;
    char *s = (char *) malloc(n);
    if (!s) {
        return NULL;
    }
    snprintf(s, n, "%s%s", prefix, rest);
    return s;
}
