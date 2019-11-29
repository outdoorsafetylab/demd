#ifndef ELEVATION_H
#define ELEVATION_H

struct evhttp_request;

void elevation_request_cb(struct evhttp_request *req, void *arg);

#endif // ELEVATION_H
