#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include <gdal.h>
#include <cpl_conv.h>

#include "paths.h"
#include "dataset.h"
#include "index.h"

static char *indexDirOf(const char *path);
static char *indexResolve(const char *dir, const char *entry);
static int indexParseEntry(const char *line, struct index_entry *out, const char *dir,
                           const char *file, size_t lineno);
static char *indexTimestamp(void);
static int indexBoundsOf(const char *path, OGRSpatialReferenceH hReqSRS,
                         double *t, double *l, double *b, double *r);

int IndexLooksLikeIndex(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    char buf[sizeof(INDEX_MAGIC)];
    size_t want = strlen(INDEX_MAGIC);
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    if (got != want) {
        return 0;
    }
    return memcmp(buf, INDEX_MAGIC, want) == 0;
}

struct dem_index *IndexRead(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open index %s: %s\n", path, strerror(errno));
        return NULL;
    }
    char *dir = indexDirOf(path);
    if (!dir) {
        fclose(f);
        return NULL;
    }
    struct dem_index *idx = (struct dem_index *) calloc(1, sizeof(struct dem_index));
    if (!idx) {
        fprintf(stderr, "Failed to allocate index: %s\n", strerror(errno));
        free(dir);
        fclose(f);
        return NULL;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    size_t lineno = 0, capacity = 0;
    long declared = -1;
    int version_seen = 0, failed = 0;

    while (!failed && (len = getline(&line, &cap, f)) != -1) {
        lineno++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (line[0] == '#') {
            if (strncmp(line, INDEX_MAGIC, strlen(INDEX_MAGIC)) == 0) {
                int version = atoi(line + strlen(INDEX_MAGIC));
                if (version != 1) {
                    fprintf(stderr, "%s: unsupported index version %d\n", path, version);
                    failed = 1;
                    break;
                }
                version_seen = 1;
            } else if (strncmp(line, "#srs ", 5) == 0) {
                free(idx->srs_wkt);
                idx->srs_wkt = strdup(line + 5);
            } else if (strncmp(line, "#generated ", 11) == 0) {
                free(idx->generated);
                idx->generated = strdup(line + 11);
            } else if (strncmp(line, "#count ", 7) == 0) {
                declared = atol(line + 7);
            }
            continue;
        }
        if (!version_seen) {
            fprintf(stderr, "%s: not a demd index (missing %s header)\n", path, INDEX_MAGIC);
            failed = 1;
            break;
        }
        if (idx->n == capacity) {
            size_t grown = capacity ? capacity * 2 : 64;
            struct index_entry *bigger = (struct index_entry *)
                realloc(idx->entries, grown * sizeof(struct index_entry));
            if (!bigger) {
                fprintf(stderr, "Failed to grow index: %s\n", strerror(errno));
                failed = 1;
                break;
            }
            idx->entries = bigger;
            capacity = grown;
        }
        if (!indexParseEntry(line, &idx->entries[idx->n], dir, path, lineno)) {
            failed = 1;
            break;
        }
        idx->n++;
    }
    free(line);
    fclose(f);
    free(dir);

    if (!failed && !version_seen) {
        fprintf(stderr, "%s: not a demd index (missing %s header)\n", path, INDEX_MAGIC);
        failed = 1;
    }
    if (!failed && !idx->srs_wkt) {
        fprintf(stderr, "%s: index has no #srs header\n", path);
        failed = 1;
    }
    // A truncated index is indistinguishable from one that legitimately covers
    // less ground, and the difference is a service that quietly stops
    // answering for a region. The declared count is what tells them apart.
    if (!failed && declared < 0) {
        fprintf(stderr, "%s: index has no #count header\n", path);
        failed = 1;
    }
    if (!failed && (size_t) declared != idx->n) {
        fprintf(stderr, "%s: index declares %ld entries but holds %zu -- truncated?\n",
            path, declared, idx->n);
        failed = 1;
    }
    if (failed) {
        IndexFree(idx);
        return NULL;
    }
    return idx;
}

void IndexFree(struct dem_index *idx) {
    if (!idx) {
        return;
    }
    for (size_t i = 0; i < idx->n; i++) {
        free(idx->entries[i].path);
    }
    free(idx->entries);
    free(idx->srs_wkt);
    free(idx->generated);
    free(idx);
}

int IndexGenerate(const char *target, OGRSpatialReferenceH hReqSRS,
                  const char *srsWkt, char **paths, size_t npaths) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", target, (long) getpid());

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "Failed to create %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    char *stamp = indexTimestamp();
    fprintf(f, "%s1\n", INDEX_MAGIC);
    fprintf(f, "#srs %s\n", srsWkt);
    fprintf(f, "#generated %s\n", stamp ? stamp : "unknown");
    fprintf(f, "#count %zu\n", npaths);
    free(stamp);

    for (size_t i = 0; i < npaths; i++) {
        // A path with a newline in it would produce an index that reads back
        // as two malformed lines, so it is rejected rather than written.
        if (strchr(paths[i], '\n')) {
            fprintf(stderr, "Refusing path containing a newline: %s\n", paths[i]);
            fclose(f);
            unlink(tmp);
            return 1;
        }
        double t, l, b, r;
        if (!indexBoundsOf(paths[i], hReqSRS, &t, &l, &b, &r)) {
            // Validation is the point of doing this at build time. One
            // unreadable file fails the whole index rather than being skipped
            // into a coverage hole nobody notices until someone queries it.
            fprintf(stderr, "Failed to index: %s\n", paths[i]);
            fclose(f);
            unlink(tmp);
            return 1;
        }
        // %.17g so the doubles read back bit-for-bit. Bounds only gate a fast
        // path, and a rounded-in edge would silently drop lookups on it.
        fprintf(f, "%.17g\t%.17g\t%.17g\t%.17g\t%s\n", t, l, b, r, paths[i]);
        fprintf(stderr, "[%zu/%zu] %s\n", i + 1, npaths, paths[i]);
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fprintf(stderr, "Failed to flush %s: %s\n", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        return 1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "Failed to write %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return 1;
    }
    if (rename(tmp, target) != 0) {
        fprintf(stderr, "Failed to publish %s: %s\n", target, strerror(errno));
        unlink(tmp);
        return 1;
    }
    fprintf(stderr, "Wrote %s: %zu dataset(s)\n", target, npaths);
    return 0;
}

int IndexVerify(const char *target, OGRSpatialReferenceH hReqSRS,
                char **paths, size_t npaths) {
    struct dem_index *idx = IndexRead(target);
    if (!idx) {
        return 1;
    }
    int differs = 0;

    // Membership first, so the report names what actually moved rather than
    // reporting every entry as displaced when one was inserted at the front.
    for (size_t i = 0; i < npaths; i++) {
        int found = 0;
        for (size_t j = 0; j < idx->n && !found; j++) {
            found = strcmp(paths[i], idx->entries[j].path) == 0;
        }
        if (!found) {
            fprintf(stderr, "added:   %s\n", paths[i]);
            differs = 1;
        }
    }
    for (size_t j = 0; j < idx->n; j++) {
        int found = 0;
        for (size_t i = 0; i < npaths && !found; i++) {
            found = strcmp(paths[i], idx->entries[j].path) == 0;
        }
        if (!found) {
            fprintf(stderr, "removed: %s\n", idx->entries[j].path);
            differs = 1;
        }
    }
    if (!differs && npaths == idx->n) {
        for (size_t i = 0; i < npaths; i++) {
            if (strcmp(paths[i], idx->entries[i].path) != 0) {
                fprintf(stderr, "reordered at position %zu: index has %s, source has %s\n",
                    i + 1, idx->entries[i].path, paths[i]);
                differs = 1;
                break;
            }
        }
    }

    for (size_t i = 0; i < npaths; i++) {
        for (size_t j = 0; j < idx->n; j++) {
            if (strcmp(paths[i], idx->entries[j].path) != 0) {
                continue;
            }
            double t, l, b, r;
            if (!indexBoundsOf(paths[i], hReqSRS, &t, &l, &b, &r)) {
                fprintf(stderr, "unreadable: %s\n", paths[i]);
                differs = 1;
                break;
            }
            if (t != idx->entries[j].top || l != idx->entries[j].left
                    || b != idx->entries[j].bottom || r != idx->entries[j].right) {
                fprintf(stderr, "bounds changed: %s\n", paths[i]);
                fprintf(stderr, "  index:  (%.17g,%.17g,%.17g,%.17g)\n",
                    idx->entries[j].top, idx->entries[j].left,
                    idx->entries[j].bottom, idx->entries[j].right);
                fprintf(stderr, "  source: (%.17g,%.17g,%.17g,%.17g)\n", t, l, b, r);
                differs = 1;
            }
            break;
        }
    }

    if (differs) {
        fprintf(stderr, "%s is out of date\n", target);
    } else {
        fprintf(stderr, "%s matches its sources: %zu dataset(s)\n", target, idx->n);
    }
    IndexFree(idx);
    return differs;
}

int indexBoundsOf(const char *path, OGRSpatialReferenceH hReqSRS,
                  double *t, double *l, double *b, double *r) {
    struct dataset *ds = DatasetCreate(path, hReqSRS);
    if (!ds) {
        return 0;
    }
    DatasetGetBounds(ds, t, l, b, r);
    DatasetFree(ds);
    return 1;
}

int indexParseEntry(const char *line, struct index_entry *out, const char *dir,
                    const char *file, size_t lineno) {
    double v[4];
    const char *p = line;
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        errno = 0;
        v[i] = strtod(p, &end);
        if (end == p || errno || *end != '\t') {
            fprintf(stderr, "%s:%zu: malformed bounds\n", file, lineno);
            return 0;
        }
        p = end + 1;
    }
    // The path is whatever remains, tabs included: it is the last field
    // precisely so a filename may contain one.
    if (*p == '\0') {
        fprintf(stderr, "%s:%zu: entry has no path\n", file, lineno);
        return 0;
    }
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(v[3])
            || v[0] < v[2] || v[3] < v[1]) {
        fprintf(stderr, "%s:%zu: implausible bounds (%g,%g,%g,%g)\n",
            file, lineno, v[0], v[1], v[2], v[3]);
        return 0;
    }
    out->path = indexResolve(dir, p);
    if (!out->path) {
        return 0;
    }
    out->top = v[0];
    out->left = v[1];
    out->bottom = v[2];
    out->right = v[3];
    return 1;
}

char *indexDirOf(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return strdup(".");
    }
    size_t n = (size_t) (slash - path);
    char *dir = (char *) malloc(n + 1);
    if (!dir) {
        fprintf(stderr, "Failed to allocate index directory: %s\n", strerror(errno));
        return NULL;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

// Relative entries resolve against the index, not the working directory, so
// the same volume answers the same way wherever it is mounted. Absolute paths
// and /vsi* handles are already location-independent and pass through.
char *indexResolve(const char *dir, const char *entry) {
    if (entry[0] == '/') {
        return strdup(entry);
    }
    char joined[1024];
    PathJoin(joined, sizeof(joined), dir, entry);
    return strdup(joined);
}

char *indexTimestamp(void) {
    time_t now = time(NULL);
    struct tm tm;
    if (!gmtime_r(&now, &tm)) {
        return NULL;
    }
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return NULL;
    }
    return strdup(buf);
}
