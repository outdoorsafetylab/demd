#!/usr/bin/env python3
"""Synthesize a minimal GeoTIFF in a projected CRS, with no GDAL dependency.

This exists to cover the configuration the MOI datasets actually use: the
raster is in TWD97 / TM2 zone 121 (EPSG:3826) while requests arrive in
geographic WGS84 coordinates. Only when the two sides differ does the
coordinate transform have to agree on axis order -- for a geographic raster
queried in geographic coordinates the transform is an identity and a
lat/lon swap is invisible.

    usage: mktif.py <output.tif>
"""
import struct
import sys

EPSG = 3826            # TWD97 / TM2 zone 121
WIDTH, HEIGHT = 300, 600
PIXEL = 1000.0         # metres
ORIGIN_X = 100000.0    # easting of the western edge
ORIGIN_Y = 2900000.0   # northing of the northern edge
VALUE = 5000           # uniform elevation; any in-coverage query must return it

# TIFF field types
SHORT, LONG, DOUBLE = 3, 4, 12


def main(path):
    pixels = WIDTH * HEIGHT
    data = struct.pack("<%dh" % pixels, *([VALUE] * pixels))

    pixel_scale = struct.pack("<3d", PIXEL, PIXEL, 0.0)
    tiepoint = struct.pack("<6d", 0.0, 0.0, 0.0, ORIGIN_X, ORIGIN_Y, 0.0)
    # GeoKeyDirectory: version 1.1.0, 3 keys.
    geokeys = struct.pack(
        "<16H",
        1, 1, 0, 3,
        1024, 0, 1, 1,      # GTModelTypeGeoKey        = Projected
        1025, 0, 1, 1,      # GTRasterTypeGeoKey       = PixelIsArea
        3072, 0, 1, EPSG,   # ProjectedCSTypeGeoKey
    )

    # Layout: header | raster | out-of-line tag values | IFD
    def align(n):
        return (n + 7) & ~7

    data_off = 8
    scale_off = align(data_off + len(data))
    tie_off = align(scale_off + len(pixel_scale))
    keys_off = align(tie_off + len(tiepoint))
    ifd_off = align(keys_off + len(geokeys))

    entries = [
        (256, SHORT, 1, WIDTH),
        (257, SHORT, 1, HEIGHT),
        (258, SHORT, 1, 16),
        (259, SHORT, 1, 1),           # no compression
        (262, SHORT, 1, 1),           # BlackIsZero
        (273, LONG, 1, data_off),     # StripOffsets
        (277, SHORT, 1, 1),           # SamplesPerPixel
        (278, LONG, 1, HEIGHT),       # RowsPerStrip: one strip
        (279, LONG, 1, len(data)),    # StripByteCounts
        (339, SHORT, 1, 2),           # SampleFormat: signed integer
        (33550, DOUBLE, 3, scale_off),
        (33922, DOUBLE, 6, tie_off),
        (34735, SHORT, 16, keys_off),
    ]

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
        for off, blob in ((scale_off, pixel_scale), (tie_off, tiepoint),
                          (keys_off, geokeys), (ifd_off, ifd)):
            f.seek(off)
            f.write(blob)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mktif.py <output.tif>")
    main(sys.argv[1])
