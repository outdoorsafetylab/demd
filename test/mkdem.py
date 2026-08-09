#!/usr/bin/env python3
"""Synthesize an SRTM3 .hgt tile for the end-to-end tests.

A .hgt file is raw big-endian int16 with no header; GDAL's SRTMHGT driver
derives the georeferencing entirely from the filename, so no real DEM data
needs to be downloaded to exercise the service.

The tile is split into quadrants with distinct elevations:

        120.0        120.5        121.0
  24.0  +------------+------------+
        |  NW  1000  |  NE  2000  |
  23.5  +------------+------------+
        |  SW  3000  |  SE  4000  |
  23.0  +------------+------------+

That makes the tests sensitive to axis order: a longitude/latitude swap (the
GDAL 3 default for EPSG geographic CRS) puts the query outside the tile, and a
north/south or east/west flip returns a different quadrant's value.
"""
import struct
import sys

N = 1201  # 3 arc-second tile
NW, NE, SW, SE = 1000, 2000, 3000, 4000


def main(path):
    half = N // 2
    north = struct.pack(">%dh" % N, *([NW] * half + [NE] * (N - half)))
    south = struct.pack(">%dh" % N, *([SW] * half + [SE] * (N - half)))
    with open(path, "wb") as f:
        for row in range(N):
            # Row 0 is the northern edge.
            f.write(north if row < half else south)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mkdem.py <output.hgt>")
    main(sys.argv[1])
