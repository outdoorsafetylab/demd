#ifndef DATASET_H_
#define DATASET_H_

struct dataset;
struct dataset *DatasetCreate(const char *, const char *);
void DatasetFree(struct dataset *);
const char *DatasetFilename(struct dataset *ctx);
void DatasetGetBounds(struct dataset *, double *t, double *l, double *b, double *r);
double DatasetGetAltitude(struct dataset *, double, double);

#endif // DATASET_H_
