#!/usr/bin/env python3
"""End-to-end tests for demd.

Starts the given binary against a synthetic DEM tile, exercises the REST API,
and fails on any unexpected response, on a crash, or on any sanitizer report
in the server's output.

    usage: run.py <path-to-demd>
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
AUTH = "Bearer test-token"

failures = []
checks = 0


def check(name, expect, actual):
    global checks
    checks += 1
    if expect == actual:
        print("  ok    %-46s %s" % (name, actual))
    else:
        print("  FAIL  %-46s expected %r, got %r" % (name, expect, actual))
        failures.append(name)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, binary, dems, *args):
        if isinstance(dems, str):
            dems = [dems]
        self.port = free_port()
        self.log = tempfile.TemporaryFile(mode="w+")
        self.proc = subprocess.Popen(
            [binary, "-p", str(self.port), *args, *dems],
            stdout=self.log, stderr=subprocess.STDOUT,
        )
        self.url = "http://127.0.0.1:%d/v1/elevations" % self.port
        for _ in range(100):
            if self.proc.poll() is not None:
                raise RuntimeError("server exited on startup:\n" + self.output())
            try:
                with socket.create_connection(("127.0.0.1", self.port), 0.2):
                    return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server never bound a port:\n" + self.output())

    def post(self, body, method="POST", headers=None):
        """Returns (status, parsed-body-or-None). Never raises for HTTP errors."""
        data = body if isinstance(body, bytes) else body.encode()
        req = urllib.request.Request(self.url, data=data, method=method)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                raw = resp.read().decode().strip()
                try:
                    return resp.status, json.loads(raw)
                except ValueError:
                    return resp.status, raw
        except urllib.error.HTTPError as e:
            return e.code, None
        except urllib.error.URLError as e:
            return None, str(e)

    def alive(self):
        return self.proc.poll() is None

    def bounds(self):
        """(top, left, bottom, right) from the startup line, in the request SRS."""
        m = re.search(r"Dataset \d+ loaded: \S+ => \(([-\d.,]+)\)", self.output())
        if not m:
            return None
        return tuple(float(v) for v in m.group(1).split(","))

    def output(self):
        self.log.seek(0)
        return self.log.read()

    def shutdown(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            return self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return "timeout"


def run_cli(binary, *args):
    """Run to completion and return (exit code, combined output)."""
    p = subprocess.run([binary, *args], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=60)
    return p.returncode, p.stdout.decode(errors="replace")


def check_no_sanitizer(name, output):
    for marker in ("AddressSanitizer", "LeakSanitizer", "runtime error:",
                   "detected memory leaks"):
        if marker in output:
            print("  FAIL  %-46s sanitizer output:" % name)
            print("\n".join("        " + l for l in output.splitlines()[-25:]))
            failures.append(name)
            return
    check(name, True, True)


def check_clean(name, server):
    """Sanitizer reports go to the server's stderr, not to any response."""
    out = server.output()
    for marker in ("AddressSanitizer", "LeakSanitizer", "runtime error:",
                   "Segmentation fault", "MemorySanitizer"):
        if marker in out:
            print("  FAIL  %-46s sanitizer/crash output:" % name)
            print("\n".join("        " + l for l in out.splitlines()[-30:]))
            failures.append(name)
            return
    check(name, True, True)


def main(binary):
    demdir = tempfile.mkdtemp(prefix="demd-test-")
    tile = os.path.join(demdir, "N23E120.hgt")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mkdem.py"), tile])

    print("== georeferencing (quadrants of the synthetic tile) ==")
    s = Server(binary, demdir)
    # Longitude first. A GDAL 3 axis-order regression puts these out of bounds.
    check("NW  [120.25, 23.75]", [1000], s.post("[[120.25,23.75]]")[1])
    check("NE  [120.75, 23.75]", [2000], s.post("[[120.75,23.75]]")[1])
    check("SW  [120.25, 23.25]", [3000], s.post("[[120.25,23.25]]")[1])
    check("SE  [120.75, 23.25]", [4000], s.post("[[120.75,23.25]]")[1])
    check("multiple points in one request", [1000, 4000],
          s.post("[[120.25,23.75],[120.75,23.25]]")[1])
    check("outside coverage -> null", [None], s.post("[[125.0,30.0]]")[1])
    check("swapped axes -> null", [None], s.post("[[23.75,120.25]]")[1])
    check("empty array", [], s.post("[]")[1])

    print("== malformed input is rejected, not fatal ==")
    cases = [
        # Each of these dereferences a non-array as an array_list in the
        # unfixed code; the first two segfault outright.
        ("[1,2]", 400),
        ("[null]", 400),
        ('["x"]', 400),
        ("[{}]", 400),
        ("[[]]", 400),
        ("[[1]]", 400),
        ("[[1,2,3]]", 400),
        # json_object_get_double() silently coerces these to 0.0.
        ('[["abc","def"]]', 400),
        ("[[null,null]]", 400),
        ("[[true,false]]", 400),
        ('[[{},[]]]', 400),
        # Not an array of anything.
        ('{"a":1}', 400),
        ("1", 400),
        ('"x"', 400),
        ("not json", 400),
        ("[[1,2]", 400),
        # Embedded NUL: the unfixed code hands this to a strlen()-based parser.
        ("[[120.25,23.75]]\x00trailing", 400),
        ("\x00\x00\x00", 400),
        # Well-formed, just repetitive.
        ("[" + "[120.25,23.75]," * 100 + "[120.25,23.75]]", 200),
    ]
    for body, expect in cases:
        check("%-38r" % body[:38], expect, s.post(body)[0])
    check("server survived malformed input", True, s.alive())

    print("== projected CRS (TWD97/EPSG:3826 raster, WGS84 request) ==")
    # This is the configuration the MOI datasets use, and the only one where
    # axis order is observable: a lat/lon swap sends the query thousands of
    # kilometres away, so the lookup falls outside the raster and returns null.
    twddir = tempfile.mkdtemp(prefix="demd-test-twd97-")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           os.path.join(twddir, "twd97.tif")])
    t = Server(binary, twddir)
    check("Mt. Jade   [120.957283, 23.47]", [5000], t.post("[[120.957283,23.47]]")[1])
    check("Taipei     [121.5, 25.03]", [5000], t.post("[[121.5,25.03]]")[1])
    check("Kaohsiung  [120.3, 22.63]", [5000], t.post("[[120.3,22.63]]")[1])
    check("swapped axes -> null", [None], t.post("[[23.47,120.957283]]")[1])
    check("far outside -> null", [None], t.post("[[0.0,0.0]]")[1])
    check_clean("no sanitizer findings (projected)", t)
    t.shutdown()

    print("== rotated geotransform ==")
    # Corner projection is order-dependent only when the geotransform has
    # nonzero shear terms; every north-up fixture above multiplies them by
    # zero and cannot see a regression there. The raster is a 800km square
    # rotated 30 degrees about the middle of the TM2 zone, so every Taiwan
    # coordinate is inside it regardless of the rotation.
    rotdir = tempfile.mkdtemp(prefix="demd-test-rot-")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           "--rotated", os.path.join(rotdir, "rotated.tif")])
    r = Server(binary, rotdir)
    # The dataset bounds are where the corner projection is actually
    # observable. They only gate a fast path -- the per-pixel range check
    # backstops every lookup -- so a corrupted envelope changes no HTTP
    # response, it just silently widens the box. Deriving the northing from
    # the already-projected easting put `left` in the Atlantic (-56.7) and
    # `top` in Mongolia (48.9) while every query below still passed.
    top, left, bottom, right = r.bounds() or (0, 0, 0, 0)
    check("rotated: bounds left  115..117", True, 114.0 < left < 118.0)
    check("rotated: bounds right 125..127", True, 124.0 < right < 128.0)
    check("rotated: bounds bottom 18..20", True, 17.0 < bottom < 21.0)
    check("rotated: bounds top   27..29", True, 27.0 < top < 30.0)
    check("rotated: Mt. Jade", [5000], r.post("[[120.957283,23.47]]")[1])
    check("rotated: Taipei", [5000], r.post("[[121.5,25.03]]")[1])
    check("rotated: Kaohsiung", [5000], r.post("[[120.3,22.63]]")[1])
    check("rotated: Hualien", [5000], r.post("[[121.6,23.99]]")[1])
    check("rotated: far outside -> null", [None], r.post("[[0.0,0.0]]")[1])
    check_clean("no sanitizer findings (rotated)", r)
    r.shutdown()

    print("== protocol ==")
    check("GET -> 405", 405, s.post("[]", method="GET")[0])
    check("empty body -> 400", 400, s.post("")[0])
    check_clean("no sanitizer findings (requests)", s)
    check("clean exit on SIGTERM", 0, s.shutdown(signal.SIGTERM))
    check_clean("no sanitizer findings (shutdown)", s)

    print("== limits ==")
    s = Server(binary, demdir, "-m", "10")
    check("11 points -> 413", 413, s.post("[" + "[120.25,23.75]," * 10 + "[120.25,23.75]]")[0])
    check("10 points -> 200", 200, s.post("[" + "[120.25,23.75]," * 9 + "[120.25,23.75]]")[0])
    check("oversized body -> rejected", True, s.post("[" + "[120.25,23.75]," * 50000 + "[1,2]]")[0] != 200)
    check("server survived oversized body", True, s.alive())
    check_clean("no sanitizer findings (limits)", s)
    s.shutdown()

    print("== dataset precedence ==")
    # Real deployments layer a current dataset over an older one that still
    # covers ground the new survey dropped. Which one answers must depend on
    # the order the operator gave, not on directory iteration order.
    hi = tempfile.mkdtemp(prefix="demd-test-hi-")
    lo = tempfile.mkdtemp(prefix="demd-test-lo-")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mkdem.py"),
                           "--hole-nw", os.path.join(hi, "N23E120.hgt")])
    subprocess.check_call([sys.executable, os.path.join(HERE, "mkdem.py"),
                           "--fill", "9999", os.path.join(lo, "N23E120.hgt")])

    p = Server(binary, [hi, lo])
    check("hi first: NE from hi", [2000], p.post("[[120.75,23.75]]")[1])
    check("hi first: NW hole filled by lo", [9999], p.post("[[120.25,23.75]]")[1])
    check("hi first: SW from hi", [3000], p.post("[[120.25,23.25]]")[1])
    p.shutdown()

    p = Server(binary, [lo, hi])
    check("lo first: NE from lo", [9999], p.post("[[120.75,23.75]]")[1])
    check("lo first: NW from lo", [9999], p.post("[[120.25,23.75]]")[1])
    p.shutdown()

    # A single file argument still works, and mixes with directories.
    p = Server(binary, [os.path.join(hi, "N23E120.hgt"), lo])
    check("file arg then dir: NE from file", [2000], p.post("[[120.75,23.75]]")[1])
    check("file arg then dir: NW from dir", [9999], p.post("[[120.25,23.75]]")[1])
    p.shutdown()

    # Within one directory the order is alphabetical, not readdir order.
    both = tempfile.mkdtemp(prefix="demd-test-both-")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           "--value", "1111", os.path.join(both, "a.tif")])
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           "--value", "2222", os.path.join(both, "b.tif")])
    p = Server(binary, both)
    check("same dir: a.tif before b.tif", [1111], p.post("[[120.957283,23.47]]")[1])
    check_clean("no sanitizer findings (precedence)", p)
    p.shutdown()

    print("== argument validation ==")
    # 0 legitimately means "unlimited", so anything that silently converts to
    # 0 disables both the point cap and the body-size cap.
    for bad in ("", "abc", "10x", " ", "-1", "99999999999999999999", "1e5"):
        code, _ = run_cli(binary, "-m", bad, demdir)
        check("-m %-24r rejected" % bad, True, code != 0)
    for good in ("0", "1", "100000"):
        s2 = Server(binary, demdir, "-m", good)
        check("-m %-24r accepted" % good, True, s2.alive())
        s2.shutdown()

    print("== invalid SRS (the path that used to leak) ==")
    # sanitizeSRS() returned early on OSRSetFromUserInput() failure without
    # destroying its spatial reference. Nothing else in the suite reaches it.
    code, out = run_cli(binary, "-s", "not-a-spatial-reference", demdir)
    check("invalid -s exits nonzero", True, code != 0)
    check_no_sanitizer("invalid -s leaves no leak", out)
    code, out = run_cli(binary, "-s", "EPSG:999999", demdir)
    check("unknown EPSG exits nonzero", True, code != 0)
    check_no_sanitizer("unknown EPSG leaves no leak", out)
    code, out = run_cli(binary, "/nonexistent-dem-path")
    check("missing DEM path exits nonzero", True, code != 0)
    check_no_sanitizer("missing DEM path leaves no leak", out)

    print("== auth ==")
    s = Server(binary, demdir, "-A", AUTH)
    check("no credentials -> 401", 401, s.post("[[120.25,23.75]]")[0])
    check("wrong credentials -> 401", 401,
          s.post("[[120.25,23.75]]", headers={"Authorization": "Bearer nope"})[0])
    check("correct credentials -> 200", 200,
          s.post("[[120.25,23.75]]", headers={"Authorization": AUTH})[0])
    check_clean("no sanitizer findings (auth)", s)
    check("clean exit on SIGINT", 0, s.shutdown(signal.SIGINT))

    print()
    if failures:
        print("FAILED %d of %d checks:" % (len(failures), checks))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASSED all %d checks" % checks)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: run.py <path-to-demd>")
    sys.exit(main(sys.argv[1]))
