#ifndef CONTEXT_H_
#define CONTEXT_H_

#include <stddef.h>

struct context;
// Paths are consulted in the order given, and the files inside a directory in
// sorted order. The first dataset that yields a value for a coordinate wins,
// so earlier paths take precedence over later ones.
struct context *ContextCreate(const char **paths, size_t npaths, const char *, const char *);
void ContextFree(struct context *);
const char *ContextAuth(struct context *ctx);
int ContextEmpty(struct context *ctx);
void ContextSetMaxPoints(struct context *ctx, size_t max);
size_t ContextMaxPoints(struct context *ctx);
void ContextSetVerbose(struct context *ctx, int verbose);
int ContextVerbose(struct context *ctx);
double ContextGetAltitude(struct context *, double, double);

#endif // CONTEXT_H_
