FROM ubuntu:18.04 AS builder

RUN apt-get update
RUN apt-get install -y --no-install-recommends \
        build-essential libgdal-dev libevent-dev libjson-c-dev
RUN apt-get install -y --no-install-recommends \
        ca-certificates

COPY . /source
WORKDIR /source
RUN make clean
RUN make

FROM ubuntu:18.04 AS runtime

RUN apt-get update && \
        apt-get install -y --no-install-recommends \ 
        libevent-2.1-6 libgdal20

RUN mkdir -p /usr/sbin/
COPY --from=builder /source/demd /usr/sbin/

ARG PORT=8080
ENV PORT ${PORT}
EXPOSE ${PORT}

VOLUME ["/var/lib/dem"]

CMD ["sh", "-c", "/usr/sbin/demd -p $PORT -A \"$AUTH\" /var/lib/dem/"]
