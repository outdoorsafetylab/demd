#include <stddef.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_srs_api.h>

#include "srs.h"

char *SRSSanitize(const char *pszUserInput) {
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

void SRSUseTraditionalAxisOrder(OGRSpatialReferenceH hSRS) {
#if GDAL_VERSION_MAJOR >= 3
    OSRSetAxisMappingStrategy(hSRS, OAMS_TRADITIONAL_GIS_ORDER);
#else
    (void) hSRS;
#endif
}
