#ifndef CONTEXT_H_
#define CONTEXT_H_

struct context;
struct context *ContextCreate(const char *, const char *);
void ContextFree(struct context *);
int ContextEmpty(context *ctx);
double ContextGetAltitude(struct context *, double, double);

#endif // CONTEXT_H_
