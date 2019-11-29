# Elevation Service for DTM from Taiwan MOI

This project provides an elevation service with REST API. It is implemented in C++, with GDAL, libevent, and JSON-C.

We also built a [cloud-based service](https://outdoorsafetylab.org/elevation_api) backed by this project.

## How to Build

This project was developed on Ubuntu 18.04 LTS. You will need to install the following packages by ```apt-get``` before building it:

```shell
sudo apt-get install build-essential libgdal-dev libevent-dev libjson-c-dev
```

To build:

```shell
make
```

A executable file ```moidemd``` will be created. You can run it to see the help:

```shell
$ ./moidemd
Usage: ./moidemd [options] <DEM file>
Options:
    -a <addr> : Address to bind HTTP (default: 0.0.0.0)
    -p <port> : Port to bind HTTP (default: 80)
    -u <URI>  : URI to serve REST (default: /v1/elevations)
    -s <SRS>  : SRS of requested coordinates (default: WGS84)
```

## How to Run

If development packages was not installed, you may need the follow runtime dependency packages installed:

```shell
sudo libevent-2.1-6 libgdal20
```

Use `run` target in `Makefile` to automatically check and download [MOI DTM](https://data.gov.tw/dataset/103884) before starting the daemon:

```shell
$ make run
Serving DEMg_geoid2014_20m_20190515.tif: http://0.0.0.0:8082/v1/elevations
```

To query the elevation of Mt. Jade of this test daemon:

```shell
$ curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations
[ 3947.14 ]
```

## API Specification

See the [OpenAPI 3.0 specification](https://outdoorsafetylab.org/elevation_api.html).
