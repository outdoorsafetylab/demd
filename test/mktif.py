#!/usr/bin/env python3
"""Synthesize a minimal GeoTIFF in a projected CRS, with no GDAL dependency.

This exists to cover the configuration the MOI datasets actually use: the
raster is in TWD97 / TM2 zone 121 (EPSG:3826) while requests arrive in
geographic WGS84 coordinates. Only when the two sides differ does the
coordinate transform have to agree on axis order -- for a geographic raster
queried in geographic coordinates the transform is an identity and a
lat/lon swap is invisible.

With --rotated the georeferencing is written as a ModelTransformation matrix
carrying a real rotation, which is the only way to give the geotransform
nonzero shear terms. Corner projection is order-dependent only when those
terms are nonzero, so a north-up fixture cannot detect a regression there.

    usage: mktif.py [--rotated] <output.tif>
"""
import math
import struct
import sys

EPSG = 3826            # TWD97 / TM2 zone 121
WIDTH, HEIGHT = 300, 600
PIXEL = 1000.0         # metres
ORIGIN_X = 100000.0    # easting of the western edge
ORIGIN_Y = 2900000.0   # northing of the northern edge
VALUE = 5000           # uniform elevation; any in-coverage query must return it

# Rotated variant: a square large enough that every Taiwan coordinate stays
# inside it at any rotation, centred on the middle of the TM2 zone 121 grid.
ROT_SIZE = 800
ROT_ANGLE = 30.0
ROT_CENTER_X = 250000.0
ROT_CENTER_Y = 2600000.0

# TIFF field types
SHORT, LONG, DOUBLE = 3, 4, 12
# GeoTIFF georeferencing tags
MODEL_PIXEL_SCALE, MODEL_TRANSFORMATION, MODEL_TIEPOINT = 33550, 34264, 33922


def main(path, rotated=False, value=VALUE):
    width, height = (ROT_SIZE, ROT_SIZE) if rotated else (WIDTH, HEIGHT)
    pixels = width * height
    data = struct.pack("<%dh" % pixels, *([value] * pixels))

    if rotated:
        t = math.radians(ROT_ANGLE)
        a, b = PIXEL * math.cos(t), PIXEL * math.sin(t)
        c, d = PIXEL * math.sin(t), -PIXEL * math.cos(t)
        x0 = ROT_CENTER_X - a * width / 2 - b * height / 2
        y0 = ROT_CENTER_Y - c * width / 2 - d * height / 2
        # 4x4 affine, row-major, as GeoTIFF defines tag 34264.
        geo_tag = MODEL_TRANSFORMATION
        geo_blob = struct.pack(
            "<16d",
            a, b, 0.0, x0,
            c, d, 0.0, y0,
            0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0,
        )
    else:
        geo_tag = MODEL_PIXEL_SCALE
        geo_blob = struct.pack("<3d", PIXEL, PIXEL, 0.0)

    tiepoint = struct.pack("<6d", 0.0, 0.0, 0.0, ORIGIN_X, ORIGIN_Y, 0.0)
    # GeoKeyDirectory: version 1.1.0, 3 keys.
    geokeys = struct.pack(
        "<16H",
        1, 1, 0, 3,
        1024, 0, 1, 1,      # GTModelTypeGeoKey        = Projected
        1025, 0, 1, 1,      # GTRasterTypeGeoKey       = PixelIsArea
        3072, 0, 1, EPSG,   # ProjectedCSTypeGeoKey
    )

    # ModelTransformation is mutually exclusive with PixelScale+Tiepoint.
    blobs = [(geo_tag, DOUBLE, len(geo_blob) // 8, geo_blob)]
    if not rotated:
        blobs.append((MODEL_TIEPOINT, DOUBLE, len(tiepoint) // 8, tiepoint))
    blobs.append((34735, SHORT, len(geokeys) // 2, geokeys))
    blobs.sort(key=lambda e: e[0])

    # Layout: header | raster | out-of-line tag values | IFD
    def align(n):
        return (n + 7) & ~7

    data_off = 8
    offset = align(data_off + len(data))
    placed = []
    for tag, typ, count, blob in blobs:
        placed.append((tag, typ, count, offset, blob))
        offset = align(offset + len(blob))
    ifd_off = offset

    entries = [
        (256, SHORT, 1, width),
        (257, SHORT, 1, height),
        (258, SHORT, 1, 16),
        (259, SHORT, 1, 1),           # no compression
        (262, SHORT, 1, 1),           # BlackIsZero
        (273, LONG, 1, data_off),     # StripOffsets
        (277, SHORT, 1, 1),           # SamplesPerPixel
        (278, LONG, 1, height),       # RowsPerStrip: one strip
        (279, LONG, 1, len(data)),    # StripByteCounts
        (339, SHORT, 1, 2),           # SampleFormat: signed integer
    ] + [(tag, typ, count, off) for tag, typ, count, off, _ in placed]
    entries.sort(key=lambda e: e[0])

    ifd = struct.pack("<H", len(entries))
    for tag, typ, count, value in entries:
        # Values of 4 bytes or fewer live inline; on little-endian a SHORT
        # occupies the low half of the field, which is what packing as a
        # 32-bit little-endian word produces.
        ifd += struct.pack("<HHLL", tag, typ, count, value)
    ifd += struct.pack("<L", 0)  # no next IFD

    with open(path, "wb") as f:
        f.write(struct.pack("<2sHL", b"II", 42, ifd_off))
        f.write(data)
        for _, _, _, off, blob in placed:
            f.seek(off)
            f.write(blob)
        f.seek(ifd_off)
        f.write(ifd)


if __name__ == "__main__":
    args = sys.argv[1:]
    rotated = "--rotated" in args
    args = [a for a in args if a != "--rotated"]
    value = VALUE
    if "--value" in args:
        i = args.index("--value")
        value = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit("usage: mktif.py [--rotated] [--value N] <output.tif>")
    main(args[0], rotated, value)
