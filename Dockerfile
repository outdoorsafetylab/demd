FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config ca-certificates python3 \
        libgdal-dev libevent-dev libjson-c-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /source
WORKDIR /source
RUN make clean && make
# Fail the build rather than ship a binary that does not pass its own tests.
RUN make test

FROM ubuntu:24.04 AS runtime

ARG GIT_HASH=""
ARG GIT_TAG=""
LABEL org.opencontainers.image.title="demd" \
      org.opencontainers.image.description="Elevation service hosting DTM files" \
      org.opencontainers.image.source="https://github.com/outdoorsafetylab/demd" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.revision="${GIT_HASH}" \
      org.opencontainers.image.version="${GIT_TAG}"

RUN apt-get update && apt-get install -y --no-install-recommends \
        libgdal34t64 libevent-2.1-7t64 libjson-c5 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /source/demd /usr/sbin/demd

ARG PORT=8080
ENV PORT=${PORT}
EXPOSE ${PORT}

VOLUME ["/var/lib/dem"]

# Ubuntu 24.04 ships an unprivileged `ubuntu` account at uid 1000.
USER ubuntu

# `exec` so demd becomes PID 1 and receives SIGTERM directly; otherwise the
# shell swallows it and the shutdown path never runs.
CMD ["sh", "-c", "exec /usr/sbin/demd -p $PORT -A \"$AUTH\" /var/lib/dem/"]
