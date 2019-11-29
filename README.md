# Elevation Service for DTM Files

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

A executable file `demd` will be created. You can run it to see the help:

```shell
Usage: ./demd [options] <DEM file or directory of DEM files>
Options:
    -a <addr> : Address to bind HTTP (default: 0.0.0.0)
    -p <port> : Port to bind HTTP (default: 80)
    -u <URI>  : URI to serve REST (default: /v1/elevations)
    -s <SRS>  : SRS of requested coordinates (default: WGS84)
```

## How to Run

If development packages was not installed, you may need the follow runtime dependency packages installed:

```shell
sudo apt-get install libevent-2.1-6 libgdal20
```

Use `run` target in `Makefile` to automatically download sample DEM files before starting the daemon:

```shell
$ make run
./demd -p 8082 dem
Dataset loaded: dem/N21E120.hgt => (22.000417,119.999583,20.999583,121.000417)
Dataset loaded: dem/N23E121.hgt => (24.000417,120.999583,22.999583,122.000417)
Dataset loaded: dem/N20E121.hgt => (21.000417,120.999583,19.999583,122.000417)
Dataset loaded: dem/N23E120.hgt => (24.000417,119.999583,22.999583,121.000417)
Dataset loaded: dem/N20E122.hgt => (21.000417,121.999583,19.999583,123.000417)
Dataset loaded: dem/N22E121.hgt => (23.000417,120.999583,21.999583,122.000417)
Dataset loaded: dem/N22E120.hgt => (23.000417,119.999583,21.999583,121.000417)
Dataset loaded: dem/N21E121.hgt => (22.000417,120.999583,20.999583,122.000417)
Serving http://0.0.0.0:8082/v1/elevations
```

To query the elevation of Mt. Jade, highest peak of Taiwan, of this test daemon:

```shell
$ curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations
[ 3917 ]
```

## API Specification

See the [OpenAPI 3.0 specification](https://outdoorsafetylab.org/elevation_api.html).
