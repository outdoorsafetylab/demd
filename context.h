#ifndef CONTEXT_H_
#define CONTEXT_H_

struct context;
struct context *ContextCreate(const char *, const char *, const char *);
void ContextFree(struct context *);
const char *ContextAuth(struct context *ctx);
int ContextEmpty(struct context *ctx);
double ContextGetAltitude(struct context *, double, double);

#endif // CONTEXT_H_
