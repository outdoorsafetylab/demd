#ifndef DATASET_H_
#define DATASET_H_

#include <ogr_srs_api.h>

struct dataset;

// Opens the file now. Bounds come from its geotransform, and a file that
// cannot be read is rejected here -- this is the path a directory scan uses,
// and it is deliberately fail-fast.
struct dataset *DatasetCreate(const char *filename, OGRSpatialReferenceH hReqSRS);

// Defers the open. Bounds come from the caller (an index built earlier), and
// the file is not touched until a lookup falls inside them.
struct dataset *DatasetCreateIndexed(const char *filename, OGRSpatialReferenceH hReqSRS,
    double top, double left, double bottom, double right);

void DatasetFree(struct dataset *);
const char *DatasetFilename(struct dataset *);
void DatasetGetBounds(struct dataset *, double *t, double *l, double *b, double *r);

// Pure bbox test. Never opens the file, so it is safe to run across every
// dataset for every point.
int DatasetContains(struct dataset *, double x, double y);

int DatasetIsOpen(struct dataset *);
// Opens if needed. Returns 0 on failure, which includes "a previous failure's
// backoff has not expired yet" -- that case is silent by design.
int DatasetOpen(struct dataset *);
// Releases the file handle and everything derived from it. The dataset stays
// usable: its bounds survive, and the next lookup inside them reopens it.
void DatasetClose(struct dataset *);

double DatasetGetAltitude(struct dataset *, double, double);

#endif // DATASET_H_
