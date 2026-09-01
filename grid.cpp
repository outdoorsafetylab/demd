#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "grid.h"

// A box covering more cells than this is not enumerated into all of them; it
// goes on the always-considered list instead. Without a cap, one raster
// spanning the world would be copied into every cell.
#define GRID_MAX_CELLS_PER_BOX 64
// Bounds on the grid itself, so a pathological extent-to-box-size ratio cannot
// ask for an unbounded allocation.
#define GRID_MAX_SIDE 4096
#define GRID_MAX_CELLS (1u << 20)

struct dataset_grid {
    double min_x, min_y, max_x, max_y;
    double cell_w, cell_h;
    size_t nx, ny;
    // CSR: cell c owns entries[offsets[c] .. offsets[c + 1]). Filled by
    // walking the boxes in index order, so every cell's list comes out
    // ascending for free -- which is what preserves precedence.
    uint32_t *offsets;
    uint32_t *entries;
    uint32_t *wide;
    size_t wide_n;
};

static int compareDouble(const void *a, const void *b);
static double medianExtent(const struct grid_box *boxes, size_t n, int vertical);
static size_t cellIndex(const struct dataset_grid *g, double v, double min, double cell, size_t n);
static void boxCells(const struct dataset_grid *g, const struct grid_box *b,
                     size_t *x0, size_t *x1, size_t *y0, size_t *y1);

struct dataset_grid *GridBuild(const struct grid_box *boxes, size_t n) {
    if (n == 0) {
        return NULL;
    }
    struct dataset_grid *g = (struct dataset_grid *) calloc(1, sizeof(struct dataset_grid));
    if (!g) {
        fprintf(stderr, "Failed to allocate lookup grid: %s\n", strerror(errno));
        return NULL;
    }
    g->min_x = boxes[0].left;
    g->max_x = boxes[0].right;
    g->min_y = boxes[0].bottom;
    g->max_y = boxes[0].top;
    for (size_t i = 1; i < n; i++) {
        if (boxes[i].left < g->min_x) g->min_x = boxes[i].left;
        if (boxes[i].right > g->max_x) g->max_x = boxes[i].right;
        if (boxes[i].bottom < g->min_y) g->min_y = boxes[i].bottom;
        if (boxes[i].top > g->max_y) g->max_y = boxes[i].top;
    }
    double span_x = g->max_x - g->min_x;
    double span_y = g->max_y - g->min_y;
    if (!(span_x > 0)) span_x = 1;
    if (!(span_y > 0)) span_y = 1;

    // Size the cells after the datasets, not after the coordinate system: the
    // bounds are in whatever SRS requests arrive in, so degrees and metres are
    // both possible and a fixed cell size would be wrong in one of them. The
    // median rather than the mean, so a handful of large rasters among many
    // tiles does not coarsen the grid for all of them.
    double w = medianExtent(boxes, n, 0);
    double h = medianExtent(boxes, n, 1);
    if (!(w > 0)) w = span_x;
    if (!(h > 0)) h = span_y;

    double nxd = ceil(span_x / w);
    double nyd = ceil(span_y / h);
    g->nx = (nxd < 1) ? 1 : (nxd > GRID_MAX_SIDE ? GRID_MAX_SIDE : (size_t) nxd);
    g->ny = (nyd < 1) ? 1 : (nyd > GRID_MAX_SIDE ? GRID_MAX_SIDE : (size_t) nyd);
    while (g->nx * g->ny > GRID_MAX_CELLS) {
        if (g->nx >= g->ny) {
            g->nx = g->nx / 2 + 1;
        } else {
            g->ny = g->ny / 2 + 1;
        }
    }
    // Nudged outward so a coordinate exactly on the far edge still lands in
    // the last cell rather than one past it.
    g->cell_w = span_x / (double) g->nx;
    g->cell_h = span_y / (double) g->ny;
    if (!(g->cell_w > 0)) g->cell_w = 1;
    if (!(g->cell_h > 0)) g->cell_h = 1;

    size_t ncells = g->nx * g->ny;
    g->offsets = (uint32_t *) calloc(ncells + 1, sizeof(uint32_t));
    uint32_t *counts = (uint32_t *) calloc(ncells, sizeof(uint32_t));
    g->wide = (uint32_t *) calloc(n, sizeof(uint32_t));
    if (!g->offsets || !counts || !g->wide) {
        fprintf(stderr, "Failed to allocate lookup grid: %s\n", strerror(errno));
        free(counts);
        GridFree(g);
        return NULL;
    }

    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        size_t x0, x1, y0, y1;
        boxCells(g, &boxes[i], &x0, &x1, &y0, &y1);
        size_t covered = (x1 - x0 + 1) * (y1 - y0 + 1);
        if (covered > GRID_MAX_CELLS_PER_BOX) {
            g->wide[g->wide_n++] = (uint32_t) i;
            continue;
        }
        for (size_t cy = y0; cy <= y1; cy++) {
            for (size_t cx = x0; cx <= x1; cx++) {
                counts[cy * g->nx + cx]++;
                total++;
            }
        }
    }
    for (size_t c = 0; c < ncells; c++) {
        g->offsets[c + 1] = g->offsets[c] + counts[c];
    }
    g->entries = (uint32_t *) calloc(total ? total : 1, sizeof(uint32_t));
    uint32_t *cursor = (uint32_t *) calloc(ncells, sizeof(uint32_t));
    if (!g->entries || !cursor) {
        fprintf(stderr, "Failed to allocate lookup grid: %s\n", strerror(errno));
        free(counts);
        free(cursor);
        GridFree(g);
        return NULL;
    }
    memcpy(cursor, g->offsets, ncells * sizeof(uint32_t));
    // Ascending i, so each cell's slice ends up sorted without sorting it.
    for (size_t i = 0; i < n; i++) {
        size_t x0, x1, y0, y1;
        boxCells(g, &boxes[i], &x0, &x1, &y0, &y1);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) > GRID_MAX_CELLS_PER_BOX) {
            continue;
        }
        for (size_t cy = y0; cy <= y1; cy++) {
            for (size_t cx = x0; cx <= x1; cx++) {
                g->entries[cursor[cy * g->nx + cx]++] = (uint32_t) i;
            }
        }
    }
    free(counts);
    free(cursor);
    return g;
}

void GridStats(const struct dataset_grid *g, size_t *nx, size_t *ny, size_t *oversized) {
    *nx = g->nx;
    *ny = g->ny;
    *oversized = g->wide_n;
}

void GridFree(struct dataset_grid *g) {
    if (!g) {
        return;
    }
    free(g->offsets);
    free(g->entries);
    free(g->wide);
    free(g);
}

void GridBegin(const struct dataset_grid *g, double x, double y, struct grid_cursor *cur) {
    cur->cell = NULL;
    cur->cell_n = 0;
    cur->wide = g->wide;
    cur->wide_n = g->wide_n;
    // Outside the union of every box, so nothing can contain it -- including
    // the wide list, whose boxes are inside that union by construction. This
    // is the case a linear scan pays the most for and the grid answers
    // outright.
    if (!isfinite(x) || !isfinite(y)
            || x < g->min_x || x > g->max_x || y < g->min_y || y > g->max_y) {
        cur->wide_n = 0;
        return;
    }
    size_t cx = cellIndex(g, x, g->min_x, g->cell_w, g->nx);
    size_t cy = cellIndex(g, y, g->min_y, g->cell_h, g->ny);
    size_t c = cy * g->nx + cx;
    cur->cell = g->entries + g->offsets[c];
    cur->cell_n = g->offsets[c + 1] - g->offsets[c];
}

int GridNext(struct grid_cursor *cur, size_t *index) {
    // Two ascending lists merged on the fly. Concatenating them would be
    // simpler and would silently reorder precedence.
    if (cur->cell_n == 0 && cur->wide_n == 0) {
        return 0;
    }
    if (cur->wide_n == 0 || (cur->cell_n > 0 && *cur->cell <= *cur->wide)) {
        *index = *cur->cell++;
        cur->cell_n--;
    } else {
        *index = *cur->wide++;
        cur->wide_n--;
    }
    return 1;
}

size_t cellIndex(const struct dataset_grid *g, double v, double min, double cell, size_t n) {
    (void) g;
    double d = floor((v - min) / cell);
    if (!(d > 0)) {
        return 0;
    }
    if (d >= (double) n) {
        return n - 1;
    }
    return (size_t) d;
}

void boxCells(const struct dataset_grid *g, const struct grid_box *b,
              size_t *x0, size_t *x1, size_t *y0, size_t *y1) {
    *x0 = cellIndex(g, b->left, g->min_x, g->cell_w, g->nx);
    *x1 = cellIndex(g, b->right, g->min_x, g->cell_w, g->nx);
    *y0 = cellIndex(g, b->bottom, g->min_y, g->cell_h, g->ny);
    *y1 = cellIndex(g, b->top, g->min_y, g->cell_h, g->ny);
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

int compareDouble(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

double medianExtent(const struct grid_box *boxes, size_t n, int vertical) {
    double *v = (double *) calloc(n, sizeof(double));
    if (!v) {
        return 0;
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        double d = vertical ? (boxes[i].top - boxes[i].bottom)
                            : (boxes[i].right - boxes[i].left);
        if (isfinite(d) && d > 0) {
            v[kept++] = d;
        }
    }
    double median = 0;
    if (kept > 0) {
        qsort(v, kept, sizeof(double), compareDouble);
        median = v[kept / 2];
    }
    free(v);
    return median;
}
