#ifndef GRID_H_
#define GRID_H_

#include <stddef.h>
#include <stdint.h>

// A uniform grid over the datasets' bounding boxes, so a lookup can consider
// the few datasets whose box could contain a point instead of all of them.
//
// The list walk it replaces is O(datasets) per point, which is free at six
// datasets and is not at the tens of thousands an index makes possible: the
// scan does not care how many datasets a request touches, only how far down
// the list the answering one sits.
//
// Precedence is the constraint that shapes the interface. Datasets are
// consulted in the order the operator gave, and the first one holding a value
// wins, so candidates have to come back in ascending insertion order -- not
// merely be the right set.
struct dataset_grid;

struct grid_box {
    double top, left, bottom, right;
};

// Boxes are indexed by their position in this array, which is the precedence
// order. Returns NULL on allocation failure, which the caller treats as fatal:
// the only alternative is a second lookup path that no test exercises and that
// would therefore run for the first time during the memory exhaustion that
// produced it.
struct dataset_grid *GridBuild(const struct grid_box *boxes, size_t n);
void GridFree(struct dataset_grid *grid);

// The grid's shape, reported at startup. Whether a box was too wide to
// enumerate is otherwise invisible, and a test cannot pin a path it cannot
// see: consulting the oversized list in the wrong order returns a different
// answer, and consulting it in the right order looks exactly like not having
// one at all.
void GridStats(const struct dataset_grid *grid, size_t *nx, size_t *ny, size_t *oversized);

// Walks the candidates for one point in ascending index order. No allocation:
// the cursor merges the two lists the grid keeps (the cell's own, and the
// boxes too large to be worth enumerating cells for) as it goes.
struct grid_cursor {
    const uint32_t *cell;
    size_t cell_n;
    const uint32_t *wide;
    size_t wide_n;
};

void GridBegin(const struct dataset_grid *grid, double x, double y, struct grid_cursor *cur);
// Returns 0 when the candidates are exhausted.
int GridNext(struct grid_cursor *cur, size_t *index);

#endif // GRID_H_
