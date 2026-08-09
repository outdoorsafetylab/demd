# Elevation service hosting DTM files

This provides a REST API service for querying elevations defined in DTM files. It is implemented in C++, with GDAL, libevent, and JSON-C. If you are familiar with `gdal` commands, you could imagine this as a daemonized and enhanced `gdallocationinfo`.

# Why not just use `gdallocationinfo`?

Yes, you can just use `gdallocationinfo`. But every command of it forks a new process, and then open a DTM file just to query a single elevation for you. On the contrary, `demd` hosts multiple DTM files as a daemon (server, or service) and are capable to query multiple elevations in the same API request. If you care about performance and resource utilization, it will be a good investment to run a micro elevation service.

# How to use (as docker container)

1. Prepare DTM files in a folder. If you don't have any DTM, you can download some from [Viewfinder Panoramas](http://viewfinderpanoramas.org/). The fastest way is to pick up from its [world map](http://www.viewfinderpanoramas.org/Coverage%20map%20viewfinderpanoramas_org3.htm).
1. Start a container running our public [docker image](https://hub.docker.com/r/outdoorsafetylab/demd) (replace `/path/to/your/dtms` with the real path of your DTM files):
    ```shell
    docker run -it --rm -p 8082:8082 -v "/path/to/your/dtms:/var/lib/dem" outdoorsafetylab/demd
    ```
1. Try to query a elevation of somewhere you are familiar with. For example, to query the elevation of Mt. Jade, highest peak of Taiwan:
    ```shell
    curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations
    ```
1. You can also cross-check with `gdallocationinfo` command:
    ```shell
    gdallocationinfo -wgs84 -valonly N23E120.hgt 120.957283 23.47
    ```

# How to build

This project is built and tested on Ubuntu 24.04 LTS (GDAL 3.8, libevent 2.1, json-c 0.17). You will need to install the following packages by `apt-get` before building it:

```shell
sudo apt-get install build-essential pkg-config libgdal-dev libevent-dev libjson-c-dev
```

GDAL 3 or newer and json-c 0.13 or newer are required. Include paths and link
flags are resolved through `gdal-config` and `pkg-config`, so no distribution
specific paths are hard-coded.

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
    -A <auth> : 'Authorization' header to control access, 401 status will be replied if not matched. (default: none)
    -m <max>  : Maximum number of points per request, 0 for unlimited (default: 100000)
    -q        : Do not log every lookup
```

# How to test

The test suite synthesizes its own DEM tiles, so no data needs to be downloaded:

```shell
make test           # end-to-end tests against a normal build
make test/sanitize  # the same tests under AddressSanitizer and UBSan
```

`make test/sanitize` is the one that matters for memory safety — `-Wall -Wextra`
does not detect the class of bug the suite guards against. Both run in CI.

# How to run

If development packages was not installed, you may need the follow runtime dependency packages installed:

```shell
sudo apt-get install libgdal34t64 libevent-2.1-7t64 libjson-c5
```

Or use `serve` target in `Makefile` to automatically download sample DEM files before starting the daemon:

```shell
$ make serve
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

To query the elevation of Mt. Jade, highest peak of Taiwan:

```shell
$ curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations
[ 3917 ]
```

# API specification

See the [OpenAPI 3.0 specification](https://outdoorsafetylab.org/elevation_api.html).
