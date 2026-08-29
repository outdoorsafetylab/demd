#ifndef INDEX_H_
#define INDEX_H_

#include <stddef.h>

#include <ogr_srs_api.h>

// The magic every index starts with. Recognition is by content, not by
// filename, so `demd -w /anywhere/world.idx` produces something the server can
// actually be handed back.
#define INDEX_MAGIC "#demd-index "

struct index_entry {
    double top, left, bottom, right;
    char *path;
};

struct dem_index {
    char *srs_wkt;     // the request SRS the bounds were computed in
    char *generated;   // ISO-8601 UTC, or NULL if the index does not say
    struct index_entry *entries;
    size_t n;
};

// Cheap content sniff, safe to run on any path argument.
int IndexLooksLikeIndex(const char *path);

// Reads and validates an index. Relative entry paths resolve against the index
// file's own directory, so a data volume can be mounted anywhere. Prints the
// reason and returns NULL on any problem: a malformed index is a configuration
// error, and starting anyway would serve a silently smaller world.
struct dem_index *IndexRead(const char *path);
void IndexFree(struct dem_index *idx);

// Opens every source path, computes its bounds, and publishes the index
// atomically: a temporary beside the target, then rename(). Returns 0 on
// success. On any failure -- one unreadable file is enough -- the target is
// left exactly as it was.
//
// Overwriting an index that is currently in service is the normal case, and
// the walk over every source is the slowest, most interruptible step in the
// system. Writing in place would let one timeout replace a working index with
// a half-finished one.
int IndexGenerate(const char *target, OGRSpatialReferenceH hReqSRS,
                  const char *srsWkt, char **paths, size_t npaths);

// Re-reads the sources and reports how they differ from the index: entries
// added, removed, reordered, or with changed bounds. Returns 0 when they agree.
//
// Order is compared, not just membership. Swapping two overlapping datasets
// leaves the path set, the count and every bbox untouched while changing which
// one answers first -- the kind of drift whose only symptom is a different
// number in the response.
int IndexVerify(const char *target, OGRSpatialReferenceH hReqSRS,
                char **paths, size_t npaths);

#endif // INDEX_H_
