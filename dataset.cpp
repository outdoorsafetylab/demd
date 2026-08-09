#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <ogr_srs_api.h>

#include "dataset.h"

static char *sanitizeSRS(const char *);
static int datasetGetBounds(dataset *ctx);
static int datasetGetCorner(dataset *, double *, double *);
static void useTraditionalAxisOrder(OGRSpatialReferenceH);

struct dataset {
    char *filename;
    GDALDatasetH hSrcDS;
    GDALRasterBandH hBand;
    double NoDataValue;
    OGRSpatialReferenceH hSrcSRS;
    OGRSpatialReferenceH hTrgSRS;
    char *SanitizedSRS;
    OGRCoordinateTransformationH hCT;
    OGRCoordinateTransformationH hInvCT;
    double adfGeoTransform[6];
    double adfInvGeoTransform[6];
    double top;
    double left;
    double bottom;
    double right;
};

dataset *DatasetCreate(const char *filename, const char *srs) {
    dataset *ctx = (dataset *) calloc(1, sizeof(dataset));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate dataset: %s\n", strerror(errno));
        return NULL;
    }
    ctx->hSrcDS = GDALOpen(filename, GA_ReadOnly);
    if (!ctx->hSrcDS) {
        fprintf(stderr, "Failed to open '%s': %s\n", filename, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    ctx->filename = strdup(filename);
    if (!ctx->filename) {
        fprintf(stderr, "Failed to copy filename: %s\n", strerror(errno));
        DatasetFree(ctx);
        return NULL;
    }
    int count = GDALGetRasterCount(ctx->hSrcDS);
    if (count != 1) {
        fprintf(stderr, "Unexpected number of band '%s': %d\n", filename, count);
        DatasetFree(ctx);
        return NULL;
    }
    ctx->hBand = GDALGetRasterBand(ctx->hSrcDS, 1);
    if (!ctx->hBand) {
        fprintf(stderr, "Failed to get raster band '%s': %s\n", filename, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    if (GDALDataTypeIsComplex(GDALGetRasterDataType(ctx->hBand))) {
        fprintf(stderr, "Unexpected data type '%s'\n", filename);
        DatasetFree(ctx);
        return NULL;
    }
    ctx->NoDataValue = GDALGetRasterNoDataValue(ctx->hBand, NULL);
    if (GDALGetGeoTransform(ctx->hSrcDS, ctx->adfGeoTransform) != CE_None) {
        fprintf(stderr, "Failed to get geotransform %s: %s\n", filename, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    if (!GDALInvGeoTransform(ctx->adfGeoTransform, ctx->adfInvGeoTransform)) {
        fprintf(stderr, "Failed to invert geotransform %s: %s\n", filename, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    ctx->SanitizedSRS = sanitizeSRS(srs);
    if (!ctx->SanitizedSRS) {
        fprintf(stderr, "Failed to sanitize SRS '%s': %s\n", srs, CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    ctx->hSrcSRS = OSRNewSpatialReference(ctx->SanitizedSRS);
    if (!ctx->hSrcSRS) {
        fprintf(stderr, "Failed to create source SRS: %s\n", CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    useTraditionalAxisOrder(ctx->hSrcSRS);
    ctx->hTrgSRS = OSRNewSpatialReference(GDALGetProjectionRef(ctx->hSrcDS));
    if (!ctx->hTrgSRS) {
        fprintf(stderr, "Failed to create target SRS: %s\n", CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    useTraditionalAxisOrder(ctx->hTrgSRS);
    ctx->hCT = OCTNewCoordinateTransformation(ctx->hSrcSRS, ctx->hTrgSRS);
    if (!ctx->hCT) {
        fprintf(stderr, "Failed to create coordinate transform: %s\n", CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    ctx->hInvCT = OCTNewCoordinateTransformation(ctx->hTrgSRS, ctx->hSrcSRS);
    if (!ctx->hInvCT) {
        fprintf(stderr, "Failed to inverse coordinate transform: %s\n", CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    if (!datasetGetBounds(ctx)) {
        fprintf(stderr, "Failed to get bounds: %s\n", CPLGetLastErrorMsg());
        DatasetFree(ctx);
        return NULL;
    }
    return ctx;
}

void DatasetFree(struct dataset *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->filename) {
        free(ctx->filename);
    }
    if (ctx->hCT) {
        OCTDestroyCoordinateTransformation(ctx->hCT);
    }
    if (ctx->hInvCT) {
        OCTDestroyCoordinateTransformation(ctx->hInvCT);
    }
    if (ctx->SanitizedSRS) {
        // Allocated by OSRExportToWkt(), so it belongs to CPL's allocator.
        CPLFree(ctx->SanitizedSRS);
    }
    if (ctx->hTrgSRS) {
        OSRDestroySpatialReference(ctx->hTrgSRS);
    }
    if (ctx->hSrcSRS) {
        OSRDestroySpatialReference(ctx->hSrcSRS);
    }
    if (ctx->hSrcDS) {
        GDALClose(ctx->hSrcDS);
    }
    free(ctx);
}

const char *DatasetFilename(struct dataset *ctx) {
    return ctx->filename;
}

double DatasetGetAltitude(struct dataset *ctx, double dfGeoX, double dfGeoY) {
    if (dfGeoX < ctx->left || dfGeoX > ctx->right || dfGeoY < ctx->bottom || dfGeoY > ctx->top) {
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

int datasetGetBounds(struct dataset *ctx) {
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

// GDAL 3 honours the authority-defined axis order, which makes EPSG:4326
// latitude-first. Every coordinate in this service (and in GDAL's own
// geotransforms) is longitude-first, so pin the traditional order.
void useTraditionalAxisOrder(OGRSpatialReferenceH hSRS) {
#if GDAL_VERSION_MAJOR >= 3
    OSRSetAxisMappingStrategy(hSRS, OAMS_TRADITIONAL_GIS_ORDER);
#else
    (void) hSRS;
#endif
}

char *sanitizeSRS(const char *pszUserInput) {
    OGRSpatialReferenceH hSRS;
    char *pszResult = NULL;

    CPLErrorReset();

    hSRS = OSRNewSpatialReference(NULL);
    if (!hSRS) {
        return NULL;
    }
    if (OSRSetFromUserInput(hSRS, pszUserInput) == OGRERR_NONE) {
        OSRExportToWkt(hSRS, &pszResult);
    }
    OSRDestroySpatialReference(hSRS);
    return pszResult;
}
