#ifndef SRS_H_
#define SRS_H_

#include <ogr_srs_api.h>

// Normalizes user input ("WGS84", "EPSG:4326", a WKT string) to WKT. Returns a
// CPL-allocated string to release with CPLFree(), or NULL if GDAL does not
// recognize the input as a spatial reference.
char *SRSSanitize(const char *userInput);

// GDAL 3 honours the authority-defined axis order, which makes EPSG:4326
// latitude-first. Every coordinate in this service (and in GDAL's own
// geotransforms) is longitude-first, so pin the traditional order.
void SRSUseTraditionalAxisOrder(OGRSpatialReferenceH);

#endif // SRS_H_
