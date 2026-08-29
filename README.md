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
Usage: ./demd [options] <DEM file, directory or index>...
Options:
    -a <addr> : Address to bind HTTP (default: 0.0.0.0)
    -p <port> : Port to bind HTTP (default: 80)
    -u <URI>  : URI to serve REST (default: /v1/elevations)
    -s <SRS>  : SRS of requested coordinates (default: WGS84)
    -A <auth> : 'Authorization' header to control access, 401 status will be replied if not matched. (default: none)
    -m <max>  : Maximum number of points per request, 0 for unlimited (default: 100000)
    -n <max>  : Maximum DEM files kept open at once, 0 for unlimited (default: 500)
    -q        : Do not log every lookup

Index modes (build time, not serving):
    -w <index>    : Write an index of the given sources and exit
    -W <index>    : Check an index against the given sources and exit
    -P <path>     : Print the /vsi* form of a path and exit
    --from-stdin  : Take the source paths from stdin, one per line

Paths are searched in the order given, and files within a directory in
sorted order. The first dataset holding a value for a coordinate wins,
so put higher-priority data first.

A directory holding a `demd.index` file, or an index passed directly, loads
from that index and opens each DEM only when a lookup falls inside it.
```

# Layering datasets

Elevation data is rarely a single clean set: a new survey usually supersedes an
older one over most of the area while dropping coverage somewhere the old one
still has. Pass both, newest first:

```shell
./demd -p 8082 /var/lib/dem/current /var/lib/dem/fallback
```

Every coordinate is tried against each dataset in that order, and the first one
holding a value answers. Where the current data has a hole, the fallback fills
it; everywhere else the current data wins. Ordering is explicit rather than
dependent on directory iteration, so the result does not change between hosts.

# Serving many tiles

Global coverage is not a handful of large rasters, it is tens of thousands of
1°×1° tiles. Opening every one of them at startup is one network round trip
each against object storage, which no container's startup probe will wait for.

An index removes that cost. It records each dataset's path and bounding box, so
the server can decide which file a lookup needs before opening anything, and
open only that one:

```shell
./demd -w /var/lib/dem/demd.index /var/lib/dem   # once, when the data changes
./demd -p 8082 /var/lib/dem                      # startup opens nothing
```

A directory holding a `demd.index` loads from it. A directory without one is
scanned and opened exactly as before, so an existing deployment needs no change
at all.

## Remote data

Object storage listings name their objects with a scheme GDAL does not take, so
the generator rewrites them (`-P` shows what it would produce for one path):

| listed as | indexed as |
|---|---|
| `gs://BUCKET/KEY` | `/vsigs/BUCKET/KEY` |
| `s3://BUCKET/KEY` | `/vsis3/BUCKET/KEY` |
| `https://HOST/KEY` | `/vsicurl/https://HOST/KEY` |

```shell
gsutil ls 'gs://BUCKET/PREFIX/**.tif' | sort | ./demd -w demd.index --from-stdin
```

The index itself stays on local disk; its entries point wherever the data is.

## The index is a build artifact

Generating an index opens every source, so a file that cannot be read fails the
whole run rather than becoming a coverage hole nobody notices. That check is
the reason to generate at build time: with the open deferred, a broken tile
would otherwise be discovered by whoever queried it first.

Two consequences worth knowing:

- **Regenerate whenever the tile set changes.** The index is produced by the
  job that publishes the data, not maintained by hand. `demd -W <index> <source>`
  re-reads the sources and reports anything added, removed, reordered, or with
  changed bounds; it exits nonzero when they disagree, so it can gate a deploy.
- **Order is precedence.** Two datasets covering the same ground are tried in
  the order the index lists them, so swapping two lines changes which one
  answers without changing any path or bounding box. That is why `-W` compares
  the order and not just the membership.

Publication is atomic: the generator writes beside the target and renames, so
an interrupted run leaves the index currently in service untouched.

## Bounds and the request SRS

The bounds recorded in an index are in the SRS the coordinates arrive in, not
the one the raster is stored in — they are what `-s` gates lookups against. An
index is therefore only meaningful for the `-s` it was generated with, and the
server refuses to start on a mismatch rather than answer from a box that means
something else. Equivalent spellings (`WGS84`, `EPSG:4326`) are the same SRS
and are accepted for one another.

## Open files

With the open deferred, the number of datasets stops bounding the number of
open files — every tile ever queried would otherwise stay open. `-n` caps how
many are held at once (500 by default), closing the least recently used beyond
that. A file that fails to open is retried with a backoff rather than written
off, because in object storage most failures are temporary.

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
Dataset 1 loaded: dem/N20E121.hgt => (21.000417,120.999583,19.999583,122.000417)
Dataset 2 loaded: dem/N20E122.hgt => (21.000417,121.999583,19.999583,123.000417)
Dataset 3 loaded: dem/N21E120.hgt => (22.000417,119.999583,20.999583,121.000417)
Dataset 4 loaded: dem/N21E121.hgt => (22.000417,120.999583,20.999583,122.000417)
Dataset 5 loaded: dem/N22E120.hgt => (23.000417,119.999583,21.999583,121.000417)
Dataset 6 loaded: dem/N22E121.hgt => (23.000417,120.999583,21.999583,122.000417)
Dataset 7 loaded: dem/N23E120.hgt => (24.000417,119.999583,22.999583,121.000417)
Dataset 8 loaded: dem/N23E121.hgt => (24.000417,120.999583,22.999583,122.000417)
Serving http://0.0.0.0:8082/v1/elevations
```

To query the elevation of Mt. Jade, highest peak of Taiwan:

```shell
$ curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations
[ 3917.0 ]
```

# API specification

See the [OpenAPI 3.0 specification](https://outdoorsafetylab.org/elevation_api.html).
