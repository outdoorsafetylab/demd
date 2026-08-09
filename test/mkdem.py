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
NODATA = -32768  # what the SRTMHGT driver reports as the no-data value


def main(path, fill=None, hole_nw=False):
    half = N // 2
    if fill is not None:
        row = struct.pack(">%dh" % N, *([fill] * N))
        north = south = row
    else:
        nw = NODATA if hole_nw else NW
        north = struct.pack(">%dh" % N, *([nw] * half + [NE] * (N - half)))
        south = struct.pack(">%dh" % N, *([SW] * half + [SE] * (N - half)))
    with open(path, "wb") as f:
        for row_index in range(N):
            # Row 0 is the northern edge.
            f.write(north if row_index < half else south)


if __name__ == "__main__":
    args = sys.argv[1:]
    hole_nw = "--hole-nw" in args
    args = [a for a in args if a != "--hole-nw"]
    fill = None
    if "--fill" in args:
        i = args.index("--fill")
        fill = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit("usage: mkdem.py [--hole-nw] [--fill N] <output.hgt>")
    main(args[0], fill, hole_nw)
