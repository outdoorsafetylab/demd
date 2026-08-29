#ifndef PATHS_H_
#define PATHS_H_

#include <stddef.h>

int PathExists(const char *path);
int PathIsDir(const char *path);
void PathJoin(char *dst, size_t n, const char *dir, const char *file);
int PathIsDEM(const char *name);

// Lists the DEM files in a directory as full paths, in the one order the
// service has ever used. Returns -1 on failure, otherwise the count, with *out
// set to a malloc'd array of malloc'd strings to release with PathListFree().
//
// Both the server and the index generator go through here, which is what makes
// "the index preserves the directory order" true by construction rather than
// by two implementations agreeing.
int PathListDEMs(const char *dir, char ***out);
void PathListFree(char **list, size_t n);

// Rewrites a cloud-storage URL into the /vsi* form GDAL accepts:
//
//     gs://bucket/key      -> /vsigs/bucket/key
//     s3://bucket/key      -> /vsis3/bucket/key
//     http(s)://host/key   -> /vsicurl/http(s)://host/key
//
// Anything else -- an ordinary path, or one that is already /vsi* -- is copied
// unchanged. Returns a malloc'd string, or NULL if out of memory.
//
// `gsutil ls` and `aws s3 ls` emit the scheme forms, and GDAL rejects them
// outright ("Changing the filename to /vsigs/... may help"), so a listing piped
// straight into the generator would fail on its first entry.
char *PathToVSI(const char *path);

#endif // PATHS_H_
