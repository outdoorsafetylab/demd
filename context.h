#ifndef CONTEXT_H_
#define CONTEXT_H_

#include <stddef.h>

struct context;
// Paths are consulted in the order given, and the files inside a directory in
// sorted order. The first dataset that yields a value for a coordinate wins,
// so earlier paths take precedence over later ones.
//
// A path that is a demd index, or a directory holding one, loads from it and
// defers opening each file until a lookup falls inside its bounds. max_open
// caps how many stay open at once; 0 means no cap.
struct context *ContextCreate(const char **paths, size_t npaths, const char *srs,
                              const char *auth, size_t max_open);
void ContextFree(struct context *);
const char *ContextAuth(struct context *ctx);
int ContextEmpty(struct context *ctx);
void ContextSetMaxPoints(struct context *ctx, size_t max);
size_t ContextMaxPoints(struct context *ctx);
void ContextSetVerbose(struct context *ctx, int verbose);
int ContextVerbose(struct context *ctx);
double ContextGetAltitude(struct context *, double, double);
// How many datasets the lookups since the last reset had to consider. The
// grid's whole purpose is to keep this near one however many datasets are
// loaded, and that is not visible in an answer: a grid that returned every
// dataset for every cell would produce identical responses, only slowly.
size_t ContextConsidered(struct context *ctx);
void ContextResetConsidered(struct context *ctx);

#endif // CONTEXT_H_
