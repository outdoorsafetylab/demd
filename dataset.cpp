#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <ogr_srs_api.h>

#include "srs.h"
#include "dataset.h"

static struct dataset *datasetAlloc(const char *filename, OGRSpatialReferenceH hReqSRS);
static int datasetLoad(struct dataset *, char *err, size_t errlen);
static int datasetLoadQuietly(struct dataset *, char *err, size_t errlen);
static int datasetComputeBounds(struct dataset *);
static int datasetGetCorner(struct dataset *, double *, double *);

// A file that will not open is not written off for good. These datasets may
// live in cloud storage, where 429/503/timeout and a credential that is a few
// seconds from being refreshed are ordinary and temporary; GDAL's own HTTP
// retry is off by default, so one blip arrives here as a hard failure. Writing
// that off permanently would turn a network hiccup into a tile that answers
// null for the rest of the process's life -- indistinguishable, to a caller,
// from ground nobody has data for.
//
// So: back off instead of giving up. A transient failure heals within the
// ceiling; a genuinely broken file costs one probe every few minutes.
//
// No attempt is made to sort failures into retryable and permanent.
// CPLGetLastErrorNo() is not consistent across drivers and /vsi handlers, and
// the price of getting that classification wrong is exactly the permanent
// wrong answer this exists to prevent.
#define DATASET_RETRY_CEILING 300

struct dataset {
    char *filename;
    // Borrowed. The context owns the request SRS and outlives every dataset,
    // which is what keeps one WKT string and one OGRSpatialReference in the
    // process rather than one per tile.
    OGRSpatialReferenceH hReqSRS;

    // Everything below hSrcDS is derived from the open file and is released
    // together with it by DatasetClose().
    GDALDatasetH hSrcDS;
    GDALRasterBandH hBand;
    double NoDataValue;
    OGRSpatialReferenceH hFileSRS;
    OGRCoordinateTransformationH hCT;     // request SRS -> the file's
    OGRCoordinateTransformationH hInvCT;  // the file's -> request SRS
    double adfGeoTransform[6];
    double adfInvGeoTransform[6];

    // In the request SRS, not the file's. See datasetComputeBounds().
    double top;
    double left;
    double bottom;
    double right;

    int fail_count;
    time_t next_retry;
};

struct dataset *DatasetCreate(const char *filename, OGRSpatialReferenceH hReqSRS) {
    struct dataset *ctx = datasetAlloc(filename, hReqSRS);
    if (!ctx) {
        return NULL;
    }
    char err[512];
    if (!datasetLoad(ctx, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        DatasetFree(ctx);
        return NULL;
    }
    if (!datasetComputeBounds(ctx)) {
        fprintf(stderr, "Failed to get bounds '%s': %s\n", filename, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    return ctx;
}

struct dataset *DatasetCreateIndexed(const char *filename, OGRSpatialReferenceH hReqSRS,
        double top, double left, double bottom, double right) {
    struct dataset *ctx = datasetAlloc(filename, hReqSRS);
    if (!ctx) {
        return NULL;
    }
    ctx->top = top;
    ctx->left = left;
    ctx->bottom = bottom;
    ctx->right = right;
    return ctx;
}

void DatasetFree(struct dataset *ctx) {
    if (!ctx) {
        return;
    }
    DatasetClose(ctx);
    if (ctx->filename) {
        free(ctx->filename);
    }
    free(ctx);
}

void DatasetClose(struct dataset *ctx) {
    if (ctx->hCT) {
        OCTDestroyCoordinateTransformation(ctx->hCT);
        ctx->hCT = NULL;
    }
    if (ctx->hInvCT) {
        OCTDestroyCoordinateTransformation(ctx->hInvCT);
        ctx->hInvCT = NULL;
    }
    if (ctx->hFileSRS) {
        OSRDestroySpatialReference(ctx->hFileSRS);
        ctx->hFileSRS = NULL;
    }
    if (ctx->hSrcDS) {
        GDALClose(ctx->hSrcDS);
        ctx->hSrcDS = NULL;
    }
    ctx->hBand = NULL;
}

int DatasetIsOpen(struct dataset *ctx) {
    return ctx->hSrcDS != NULL;
}

int DatasetOpen(struct dataset *ctx) {
    if (ctx->hSrcDS) {
        return TRUE;
    }
    time_t now = time(NULL);
    if (ctx->fail_count > 0 && now < ctx->next_retry) {
        return FALSE;
    }
    char err[512];
    if (datasetLoad(ctx, err, sizeof(err))) {
        ctx->fail_count = 0;
        ctx->next_retry = 0;
        return TRUE;
    }
    // A half-built dataset must not be left behind: the next attempt starts
    // from GDALOpen() and would leak whatever this one already allocated.
    DatasetClose(ctx);
    ctx->fail_count++;
    long wait = ctx->fail_count >= 31 ? DATASET_RETRY_CEILING : (1L << ctx->fail_count);
    if (wait > DATASET_RETRY_CEILING) {
        wait = DATASET_RETRY_CEILING;
    }
    ctx->next_retry = now + wait;
    // Log on powers of two. A tile that is broken for good would otherwise
    // print on every lookup that reaches it, and one that is silent entirely
    // is worse still -- the null it returns cannot be told apart from "not
    // covered" by anyone reading the response.
    if ((ctx->fail_count & (ctx->fail_count - 1)) == 0) {
        fprintf(stderr, "%s (attempt %d, next try in %lds)\n", err, ctx->fail_count, wait);
    }
    return FALSE;
}

const char *DatasetFilename(struct dataset *ctx) {
    return ctx->filename;
}

int DatasetContains(struct dataset *ctx, double x, double y) {
    return !(x < ctx->left || x > ctx->right || y < ctx->bottom || y > ctx->top);
}

double DatasetGetAltitude(struct dataset *ctx, double dfGeoX, double dfGeoY) {
    if (!DatasetContains(ctx, dfGeoX, dfGeoY)) {
        return NAN;
    }
    if (!ctx->hSrcDS) {
        return NAN;
    }
    if (!OCTTransform(ctx->hCT, 1, &dfGeoX, &dfGeoY, NULL)) {
        return NAN;
    }
    int iPixel, iLine;
    iPixel = (int) floor(
        ctx->adfInvGeoTransform[0]
        + ctx->adfInvGeoTransform[1] * dfGeoX
        + ctx->adfInvGeoTransform[2] * dfGeoY);
    iLine = (int) floor(
        ctx->adfInvGeoTransform[3]
        + ctx->adfInvGeoTransform[4] * dfGeoX
        + ctx->adfInvGeoTransform[5] * dfGeoY);
    if (iPixel < 0 || iLine < 0
            || iPixel >= GDALGetRasterXSize(ctx->hSrcDS)
            || iLine  >= GDALGetRasterYSize(ctx->hSrcDS)) {
        errno = ERANGE;
        return NAN;
    }
    double adfPixel[2];
    if (GDALRasterIO(ctx->hBand, GF_Read, iPixel, iLine, 1, 1,
                        adfPixel, 1, 1, GDT_CFloat64, 0, 0) == CE_None) {
        if (adfPixel[0] == ctx->NoDataValue) {
            return NAN;
        } else {
            return adfPixel[0];
        }
    }
    return NAN;
}

void DatasetGetBounds(struct dataset *ctx, double *t, double *l, double *b, double *r) {
    *t = ctx->top;
    *l = ctx->left;
    *b = ctx->bottom;
    *r = ctx->right;
}

struct dataset *datasetAlloc(const char *filename, OGRSpatialReferenceH hReqSRS) {
    struct dataset *ctx = (struct dataset *) calloc(1, sizeof(struct dataset));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate dataset: %s\n", strerror(errno));
        return NULL;
    }
    ctx->filename = strdup(filename);
    if (!ctx->filename) {
        fprintf(stderr, "Failed to copy filename: %s\n", strerror(errno));
        free(ctx);
        return NULL;
    }
    ctx->hReqSRS = hReqSRS;
    return ctx;
}

// Opens the file and builds everything derived from it. Reports why it failed
// through `err` rather than printing: the caller decides whether this is a
// startup error worth showing or one more backed-off retry to keep quiet.
//
// GDAL's default handler prints to stderr itself, which would defeat that --
// DatasetOpen()'s rate limiting can only govern the lines it writes. Silencing
// GDAL here does not lose anything: the message is still retrievable through
// CPLGetLastErrorMsg(), and it goes into `err`.
int datasetLoad(struct dataset *ctx, char *err, size_t errlen) {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    int ok = datasetLoadQuietly(ctx, err, errlen);
    CPLPopErrorHandler();
    return ok;
}

int datasetLoadQuietly(struct dataset *ctx, char *err, size_t errlen) {
    CPLErrorReset();
    ctx->hSrcDS = GDALOpen(ctx->filename, GA_ReadOnly);
    if (!ctx->hSrcDS) {
        snprintf(err, errlen, "Failed to open '%s': %s", ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    int count = GDALGetRasterCount(ctx->hSrcDS);
    if (count != 1) {
        snprintf(err, errlen, "Unexpected number of band '%s': %d", ctx->filename, count);
        return FALSE;
    }
    ctx->hBand = GDALGetRasterBand(ctx->hSrcDS, 1);
    if (!ctx->hBand) {
        snprintf(err, errlen, "Failed to get raster band '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    if (GDALDataTypeIsComplex(GDALGetRasterDataType(ctx->hBand))) {
        snprintf(err, errlen, "Unexpected data type '%s'", ctx->filename);
        return FALSE;
    }
    ctx->NoDataValue = GDALGetRasterNoDataValue(ctx->hBand, NULL);
    if (GDALGetGeoTransform(ctx->hSrcDS, ctx->adfGeoTransform) != CE_None) {
        snprintf(err, errlen, "Failed to get geotransform '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    if (!GDALInvGeoTransform(ctx->adfGeoTransform, ctx->adfInvGeoTransform)) {
        snprintf(err, errlen, "Failed to invert geotransform '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    ctx->hFileSRS = OSRNewSpatialReference(GDALGetProjectionRef(ctx->hSrcDS));
    if (!ctx->hFileSRS) {
        snprintf(err, errlen, "Failed to create SRS of '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    SRSUseTraditionalAxisOrder(ctx->hFileSRS);
    ctx->hCT = OCTNewCoordinateTransformation(ctx->hReqSRS, ctx->hFileSRS);
    if (!ctx->hCT) {
        snprintf(err, errlen, "Failed to create coordinate transform for '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    ctx->hInvCT = OCTNewCoordinateTransformation(ctx->hFileSRS, ctx->hReqSRS);
    if (!ctx->hInvCT) {
        snprintf(err, errlen, "Failed to inverse coordinate transform for '%s': %s",
            ctx->filename, CPLGetLastErrorMsg());
        return FALSE;
    }
    return TRUE;
}

// The envelope is expressed in the *request* SRS, not the file's: every corner
// goes through hInvCT on the way out. That is why an index records the SRS it
// was built with -- the numbers in it are only meaningful for that one.
int datasetComputeBounds(struct dataset *ctx) {
    double upperLeftX = 0, upperLeftY = 0;
    if (!datasetGetCorner(ctx, &upperLeftX, &upperLeftY)) {
        return FALSE;
    }
    double lowerLeftX = 0, lowerLeftY = GDALGetRasterYSize(ctx->hSrcDS);
    if (!datasetGetCorner(ctx, &lowerLeftX, &lowerLeftY)) {
        return FALSE;
    }
    double upperRightX = GDALGetRasterXSize(ctx->hSrcDS), upperRightY = 0;
    if (!datasetGetCorner(ctx, &upperRightX, &upperRightY)) {
        return FALSE;
    }
    double lowerRightX = GDALGetRasterXSize(ctx->hSrcDS), lowerRightY = GDALGetRasterYSize(ctx->hSrcDS);
    if (!datasetGetCorner(ctx, &lowerRightX, &lowerRightY)) {
        return FALSE;
    }
    ctx->top = upperRightY > upperLeftY ? upperRightY : upperLeftY;
    ctx->bottom = lowerRightY < lowerLeftY ? lowerRightY : lowerLeftY;
    ctx->left = upperLeftX < lowerLeftX ? upperLeftX : lowerLeftX;
    ctx->right = upperRightX > lowerRightX ? upperRightX : lowerRightX;
    return TRUE;
}

// Maps a pixel/line corner to a coordinate in the requested SRS. Both outputs
// are derived from the *original* pixel/line pair, so they need to be read
// into temporaries before either is overwritten.
int datasetGetCorner(struct dataset *ctx, double *x, double *y) {
    double pixel = *x, line = *y;
    *x = ctx->adfGeoTransform[0] + ctx->adfGeoTransform[1] * pixel
        + ctx->adfGeoTransform[2] * line;
    *y = ctx->adfGeoTransform[3] + ctx->adfGeoTransform[4] * pixel
        + ctx->adfGeoTransform[5] * line;
    double z = 0;
    return OCTTransform(ctx->hInvCT, 1, x, y, &z);
}
