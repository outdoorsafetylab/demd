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


def run_cli(binary, *args, stdin=None):
    """Run to completion and return (exit code, combined output)."""
    p = subprocess.run([binary, *args], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=120,
                       input=stdin.encode() if stdin is not None else None)
    return p.returncode, p.stdout.decode(errors="replace")


def mkdem(path, *args):
    subprocess.check_call([sys.executable, os.path.join(HERE, "mkdem.py"), *args, path])


def mktif(path, *args):
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"), *args, path])


def read_index(path):
    """Splits an index into its header lines and its entry rows."""
    head, rows = [], []
    with open(path, encoding="utf-8") as f:
        for line in f.read().splitlines():
            (head if line.startswith("#") else rows).append(line)
    return head, rows


def write_index(path, head, rows):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(head + rows) + "\n")


def index_of(dirpath):
    return os.path.join(dirpath, "demd.index")


def open_dem_files(pid):
    """The DEM files the process holds open, or None where neither /proc nor
    lsof can say. Linux and macOS answer differently and CI only runs one of
    them; going through both keeps this from being a check that first executes
    somewhere nobody is watching."""
    procfd = "/proc/%d/fd" % pid
    if os.path.isdir(procfd):
        held = []
        for name in os.listdir(procfd):
            try:
                held.append(os.readlink(os.path.join(procfd, name)))
            except OSError:
                pass  # the descriptor closed while we were looking
        return [p for p in held if p.endswith((".tif", ".hgt"))]
    try:
        p = subprocess.run(["lsof", "-p", str(pid)], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    held = []
    for line in p.stdout.decode(errors="replace").splitlines():
        parts = line.split()
        if parts and parts[-1].endswith((".tif", ".hgt")):
            held.append(parts[-1])
    return held


def check_no_sanitizer(name, output):
    for marker in ("AddressSanitizer", "LeakSanitizer", "runtime error:",
                   "detected memory leaks"):
        if marker in output:
            print("  FAIL  %-46s sanitizer output:" % name)
            print("\n".join("        " + l for l in output.splitlines()[-25:]))
            failures.append(name)
            return
    check(name, True, True)


def check_shutdown(name, server, sig=signal.SIGTERM):
    """Teardown is where the dataset list is walked and freed, so every server
    has to be stopped deliberately and inspected after it exits -- a scan taken
    while it is still running cannot see a fault in that path."""
    check("%s: clean exit" % name, 0, server.shutdown(sig))
    check_clean("%s: clean after exit" % name, server)


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
    check_shutdown("projected", t)

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
    check_shutdown("rotated", r)

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
    # Ten points either way, so the point-count check cannot be what rejects
    # these -- only the body-size cap can tell them apart. With -m 10 the cap
    # is 10*64 + 4096 bytes.
    ten = "[" + "[120.25,23.75]," * 9 + "[120.25,23.75]"
    check("10 points, 3 KB body -> 200", 200, s.post(ten + " " * 3000 + "]")[0])
    check("10 points, 8 KB body -> 413", 413, s.post(ten + " " * 8000 + "]")[0])
    check("huge body -> rejected", True, s.post("[" + "[120.25,23.75]," * 50000 + "[1,2]]")[0] != 200)
    check("server survived oversized body", True, s.alive())
    check_shutdown("limits", s)

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
    check_shutdown("precedence hi,lo", p)

    p = Server(binary, [lo, hi])
    check("lo first: NE from lo", [9999], p.post("[[120.75,23.75]]")[1])
    check("lo first: NW from lo", [9999], p.post("[[120.25,23.75]]")[1])
    check_shutdown("precedence lo,hi", p)

    # A single file argument still works, and mixes with directories.
    p = Server(binary, [os.path.join(hi, "N23E120.hgt"), lo])
    check("file arg then dir: NE from file", [2000], p.post("[[120.75,23.75]]")[1])
    check("file arg then dir: NW from dir", [9999], p.post("[[120.25,23.75]]")[1])
    check_shutdown("precedence file,dir", p)

    # Within one directory the order is alphabetical, not readdir order.
    both = tempfile.mkdtemp(prefix="demd-test-both-")
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           "--value", "1111", os.path.join(both, "a.tif")])
    subprocess.check_call([sys.executable, os.path.join(HERE, "mktif.py"),
                           "--value", "2222", os.path.join(both, "b.tif")])
    p = Server(binary, both)
    check("same dir: a.tif before b.tif", [1111], p.post("[[120.957283,23.47]]")[1])
    check_shutdown("precedence same dir", p)

    print("== argument validation ==")
    # 0 legitimately means "unlimited", so anything that silently converts to
    # 0 disables both the point cap and the body-size cap.
    for bad in ("", "abc", "10x", " ", "-1", "99999999999999999999", "1e5"):
        code, _ = run_cli(binary, "-m", bad, demdir)
        check("-m %-24r rejected" % bad, True, code != 0)
    # maxPoints * 64 + 4096 is computed as size_t; a value that overflows it
    # would wrap to a tiny body cap instead of the huge one requested.
    for huge in ("9223372036854775807", "300000000000000000"):
        code, _ = run_cli(binary, "-m", huge, demdir)
        check("-m %-24r rejected (overflow)" % huge, True, code != 0)
    for good in ("0", "1", "100000"):
        s2 = Server(binary, demdir, "-m", good)
        check("-m %-24r accepted" % good, True, s2.alive())
        check_shutdown("-m %s" % good, s2)

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
    check_shutdown("auth (SIGINT)", s, signal.SIGINT)


    print("== index: a directory with an index answers identically ==")
    # Whatever else changes, the answers must not. These are the georeferencing
    # checks from the top of the suite, re-run against an indexed copy.
    idxdir = tempfile.mkdtemp(prefix="demd-test-index-")
    mkdem(os.path.join(idxdir, "N23E120.hgt"))
    code, out = run_cli(binary, "-w", index_of(idxdir), idxdir)
    check("index written", 0, code)
    s = Server(binary, idxdir)
    check("indexed NW  [120.25, 23.75]", [1000], s.post("[[120.25,23.75]]")[1])
    check("indexed NE  [120.75, 23.75]", [2000], s.post("[[120.75,23.75]]")[1])
    check("indexed SW  [120.25, 23.25]", [3000], s.post("[[120.25,23.25]]")[1])
    check("indexed SE  [120.75, 23.25]", [4000], s.post("[[120.75,23.25]]")[1])
    check("indexed outside coverage -> null", [None], s.post("[[125.0,30.0]]")[1])
    check("indexed swapped axes -> null", [None], s.post("[[23.75,120.25]]")[1])
    # The per-dataset line reports a successful open. With an index there is
    # none to report at startup, and printing one per entry would be tens of
    # thousands of lines besides.
    check("startup reports the index, not each dataset", False,
          "Dataset 1 loaded" in s.output())
    check("startup names the index", True, "Index " in s.output())
    check_shutdown("indexed directory", s)

    # An index handed over directly is still an index, whatever it is called:
    # recognition is by content, so `-w` may write anywhere.
    named = os.path.join(tempfile.mkdtemp(prefix="demd-test-named-"), "world.idx")
    code, out = run_cli(binary, "-w", named, idxdir)
    check("index written under any name", 0, code)
    s = Server(binary, named)
    check("index passed as a file argument", [1000], s.post("[[120.25,23.75]]")[1])
    check_shutdown("named index", s)

    print("== index: its bounds are what gate lookups ==")
    # Narrow one entry to the eastern half. Bounds only gate a fast path, so a
    # server that re-derived them from the file -- i.e. opened it at startup --
    # would answer for the west anyway and pass every other check here.
    narrowdir = tempfile.mkdtemp(prefix="demd-test-narrow-")
    mkdem(os.path.join(narrowdir, "N23E120.hgt"))
    run_cli(binary, "-w", index_of(narrowdir), narrowdir)
    head, rows = read_index(index_of(narrowdir))
    fields = rows[0].split("\t")
    fields[1] = "120.5"
    write_index(index_of(narrowdir), head, ["\t".join(fields)])
    s = Server(binary, narrowdir)
    check("west of the narrowed bounds -> null", [None], s.post("[[120.25,23.75]]")[1])
    check("east of them still answers", [2000], s.post("[[120.75,23.75]]")[1])
    check_shutdown("narrowed bounds", s)

    print("== index: order is precedence ==")
    # Two datasets over identical ground. Which one answers is decided by their
    # order in the index and by nothing else -- swapping two lines changes the
    # number returned while leaving the path set, the count and every bbox
    # untouched.
    orderdir = tempfile.mkdtemp(prefix="demd-test-order-")
    mktif(os.path.join(orderdir, "a.tif"), "--value", "1111")
    mktif(os.path.join(orderdir, "b.tif"), "--value", "2222")
    run_cli(binary, "-w", index_of(orderdir), orderdir)
    head, rows = read_index(index_of(orderdir))
    check("index holds both datasets", 2, len(rows))
    s = Server(binary, orderdir)
    check("index order: a.tif answers", [1111], s.post("[[120.957283,23.47]]")[1])
    check_shutdown("index order", s)
    write_index(index_of(orderdir), head, [rows[1], rows[0]])
    s = Server(binary, orderdir)
    check("swapped index: b.tif answers", [2222], s.post("[[120.957283,23.47]]")[1])
    check_shutdown("swapped index order", s)

    print("== index: NoData falls through in index order ==")
    # The shape the production datasets actually have: two rasters over the
    # same ground, the first with holes the second still covers.
    # Both files describe the same tile, and the SRTMHGT driver takes
    # georeferencing from the filename -- so they share a name and live in
    # separate directories, exactly as the unindexed precedence test does.
    holehi = tempfile.mkdtemp(prefix="demd-test-hole-hi-")
    holelo = tempfile.mkdtemp(prefix="demd-test-hole-lo-")
    mkdem(os.path.join(holehi, "N23E120.hgt"), "--hole-nw")
    mkdem(os.path.join(holelo, "N23E120.hgt"), "--fill", "9999")
    scanned = Server(binary, [holehi, holelo])
    scan_ne = scanned.post("[[120.75,23.75]]")[1]
    scan_nw = scanned.post("[[120.25,23.75]]")[1]
    check_shutdown("fallthrough without an index", scanned)
    hole_index = os.path.join(tempfile.mkdtemp(prefix="demd-test-hole-idx-"), "demd.index")
    code, out = run_cli(binary, "-w", hole_index,
                        os.path.join(holehi, "N23E120.hgt"),
                        os.path.join(holelo, "N23E120.hgt"))
    check("index written for the layered pair", 0, code)
    s = Server(binary, hole_index)
    check("indexed fallthrough: NE matches the scan", scan_ne, s.post("[[120.75,23.75]]")[1])
    check("indexed fallthrough: NW hole matches the scan", scan_nw,
          s.post("[[120.25,23.75]]")[1])
    check("and the hole really is pierced", [9999], s.post("[[120.25,23.75]]")[1])
    check_shutdown("indexed fallthrough", s)

    print("== index: a broken entry is not touched until it is needed ==")
    # Startup silence is the assertion. An eager server would report this file
    # before binding; one that opened it and swallowed the error would still
    # have paid for the round trip.
    brokendir = tempfile.mkdtemp(prefix="demd-test-broken-")
    mkdem(os.path.join(brokendir, "N23E120.hgt"))
    run_cli(binary, "-w", index_of(brokendir), brokendir)
    head, rows = read_index(index_of(brokendir))
    broken = os.path.join(brokendir, "broken.tif")
    with open(broken, "w") as f:
        f.write("not a raster")
    head = [h if not h.startswith("#count") else "#count 2" for h in head]
    write_index(index_of(brokendir), head, rows + ["9\t118\t8\t119\t" + broken])
    s = Server(binary, brokendir)
    check("startup says nothing about the broken entry", False, "broken.tif" in s.output())
    check("other datasets answer", [1000], s.post("[[120.25,23.75]]")[1])
    check("still nothing about it", False, "broken.tif" in s.output())
    check("a lookup inside it -> null", [None], s.post("[[118.5,8.5]]")[1])
    check("and only now is it reported", True, "broken.tif" in s.output())
    check_shutdown("broken index entry", s)

    print("== index: a failed open is not permanent ==")
    # Object storage returns 429 and 503 and recovers. Writing the first
    # failure off for good would turn a blip into a tile that answers null for
    # the rest of the process's life.
    gonedir = tempfile.mkdtemp(prefix="demd-test-gone-")
    mktif(os.path.join(gonedir, "later.tif"))
    run_cli(binary, "-w", index_of(gonedir), gonedir)
    stashed = os.path.join(gonedir, "later.tif")
    with open(stashed, "rb") as f:
        saved = f.read()
    os.remove(stashed)
    s = Server(binary, gonedir)
    check("missing file -> null", [None], s.post("[[120.957283,23.47]]")[1])
    with open(stashed, "wb") as f:
        f.write(saved)
    # The first backoff is two seconds.
    time.sleep(3)
    check("once it is back, so is the answer", [5000], s.post("[[120.957283,23.47]]")[1])
    check_shutdown("recovered open", s)

    print("== index: malformed indexes are refused ==")
    baddir = tempfile.mkdtemp(prefix="demd-test-bad-")
    mkdem(os.path.join(baddir, "N23E120.hgt"))
    run_cli(binary, "-w", index_of(baddir), baddir)
    head, rows = read_index(index_of(baddir))
    truncated = os.path.join(baddir, "truncated.idx")
    write_index(truncated, [h if not h.startswith("#count") else "#count 5" for h in head], rows)
    code, out = run_cli(binary, truncated)
    check("a truncated index exits nonzero", True, code != 0)
    check("and says so", True, "truncated" in out)
    garbage = os.path.join(baddir, "garbage.idx")
    write_index(garbage, head, ["not\ta\tbounds\trow\t/x.tif"])
    check("malformed bounds exit nonzero", True, run_cli(binary, garbage)[0] != 0)
    inverted = os.path.join(baddir, "inverted.idx")
    write_index(inverted, head, ["10\t120\t20\t121\t/x.tif"])
    check("inverted bounds exit nonzero", True, run_cli(binary, inverted)[0] != 0)

    print("== index: the SRS it was built for ==")
    # The bounds are in the request SRS, so an index is only meaningful for the
    # -s it was generated with. Equivalent spellings of the same SRS must not
    # be rejected, which is why this compares with OSRIsSame() and not strcmp.
    s = Server(binary, idxdir, "-s", "EPSG:4326")
    check("EPSG:4326 against a WGS84 index still serves", [1000],
          s.post("[[120.25,23.75]]")[1])
    check_shutdown("equivalent SRS", s)
    code, out = run_cli(binary, "-s", "EPSG:3826", idxdir)
    check("a different SRS exits nonzero", True, code != 0)
    check("and explains why", True, "different SRS" in out)

    print("== index generator ==")
    for given, want in [("gs://bucket/key.tif", "/vsigs/bucket/key.tif"),
                        ("s3://bucket/key.tif", "/vsis3/bucket/key.tif"),
                        ("https://host/key.tif", "/vsicurl/https://host/key.tif"),
                        ("http://host/key.tif", "/vsicurl/http://host/key.tif"),
                        ("/vsigs/bucket/key.tif", "/vsigs/bucket/key.tif"),
                        ("/data/key.tif", "/data/key.tif"),
                        ("relative/key.tif", "relative/key.tif")]:
        code, out = run_cli(binary, "-P", given)
        check("-P %-24s" % given, want, out.strip())

    # One unreadable source fails the whole index rather than quietly leaving a
    # hole, and publication is atomic: the index in service is untouched.
    pubdir = tempfile.mkdtemp(prefix="demd-test-publish-")
    mktif(os.path.join(pubdir, "a.tif"))
    target = os.path.join(pubdir, "demd.index")
    check("first publish succeeds", 0, run_cli(binary, "-w", target, pubdir)[0])
    with open(target, "rb") as f:
        before = f.read()
    with open(os.path.join(pubdir, "z-broken.tif"), "w") as f:
        f.write("not a raster")
    code, out = run_cli(binary, "-w", target, pubdir)
    check("a corrupt source fails the index", True, code != 0)
    with open(target, "rb") as f:
        after = f.read()
    check("the published index is untouched", True, before == after)
    check("no temporary is left behind", [],
          [f for f in os.listdir(pubdir) if ".tmp." in f])

    print("== index verification ==")
    verdir = tempfile.mkdtemp(prefix="demd-test-verify-")
    mktif(os.path.join(verdir, "a.tif"), "--value", "1111")
    mktif(os.path.join(verdir, "b.tif"), "--value", "2222")
    vindex = os.path.join(verdir, "demd.index")

    def regenerate():
        run_cli(binary, "-w", vindex, verdir)
        return read_index(vindex)

    head, rows = regenerate()
    check("a fresh index verifies clean", 0, run_cli(binary, "-W", vindex, verdir)[0])
    # Reordering changes no path, no count and no bbox -- only the answer. A
    # verifier that compared sets would call this identical.
    write_index(vindex, head, [rows[1], rows[0]])
    code, out = run_cli(binary, "-W", vindex, verdir)
    check("reordering is caught", True, code != 0)
    check("reordering is named", True, "reordered" in out)
    head, rows = regenerate()
    fields = rows[0].split("\t")
    fields[0] = "99"
    write_index(vindex, head, ["\t".join(fields), rows[1]])
    code, out = run_cli(binary, "-W", vindex, verdir)
    check("changed bounds are caught", True, code != 0)
    check("changed bounds are named", True, "bounds changed" in out)
    head, rows = regenerate()
    os.remove(os.path.join(verdir, "b.tif"))
    code, out = run_cli(binary, "-W", vindex, verdir)
    check("a removed source is caught", True, code != 0)
    check("a removed source is named", True, "removed:" in out)
    mktif(os.path.join(verdir, "b.tif"), "--value", "2222")
    head, rows = regenerate()
    mktif(os.path.join(verdir, "c.tif"), "--value", "3333")
    code, out = run_cli(binary, "-W", vindex, verdir)
    check("an added source is caught", True, code != 0)
    check("an added source is named", True, "added:" in out)

    print("== lazy open over HTTP: startup makes no round trips ==")
    # The acceptance criterion behind all of this is that startup stops opening
    # every DEM. That is a claim about I/O, so this counts the I/O: a server
    # that opened the file and hid the error would be indistinguishable from a
    # lazy one by any log-based check.
    origin = None
    try:
        sys.path.insert(0, HERE)
        import httpdem
        remotedir = tempfile.mkdtemp(prefix="demd-test-remote-")
        mktif(os.path.join(remotedir, "remote.tif"))
        origin = httpdem.Origin(os.path.join(remotedir, "remote.tif"))
    except Exception as e:
        # Never a skip. This is the one check the whole change exists to make,
        # and a silent skip would leave it absent from CI while looking green.
        check("HTTP origin fixture starts", True, "failed: %r" % (e,))
    if origin:
        remote_index = os.path.join(remotedir, "demd.index")
        # A bare URL, so the gs://-style rewrite is genuinely exercised rather
        # than handed its own output.
        code, out = run_cli(binary, "-w", remote_index, "--from-stdin",
                            stdin=origin.url + "\n")
        check("remote index written", 0, code)
        with open(remote_index, encoding="utf-8") as f:
            body = f.read()
        check("the bare URL was rewritten for GDAL", True,
              "/vsicurl/" + origin.url in body)
        origin.reset()
        s = Server(binary, remote_index)
        check("binding the port made no request", 0, origin.requests)
        check("a lookup outside the bounds -> null", [None], s.post("[[0.0,0.0]]")[1])
        check("and still no request", 0, origin.requests)
        check("a lookup inside answers", [5000], s.post("[[120.957283,23.47]]")[1])
        check("which did take round trips", True, origin.requests > 0)
        check_shutdown("remote index", s)
        origin.stop()

    print("== open files stay bounded ==")
    # Deferring the open makes the dataset count unbounded, which makes open
    # descriptors the next thing to run out. Exhausting them would surface as a
    # tile that answers null -- the same shape as ground nobody has data for.
    #
    # The three tiles are stacked, not overlapping: over shared ground the
    # first dataset answers and the others are never opened, so a cap would
    # look like it worked no matter what it did.
    capdir = tempfile.mkdtemp(prefix="demd-test-cap-")
    mktif(os.path.join(capdir, "a.tif"), "--value", "1111")
    mktif(os.path.join(capdir, "b.tif"), "--value", "2222", "--shift", "-600000")
    mktif(os.path.join(capdir, "c.tif"), "--value", "3333", "--shift", "-1200000")
    run_cli(binary, "-w", index_of(capdir), capdir)
    NORTH, MIDDLE, SOUTH = "[[121.0,23.0]]", "[[121.0,18.0]]", "[[121.0,12.0]]"

    s = Server(binary, capdir, "-n", "1")
    check("stacked tiles: northern", [1111], s.post(NORTH)[1])
    check("stacked tiles: middle", [2222], s.post(MIDDLE)[1])
    check("stacked tiles: southern", [3333], s.post(SOUTH)[1])
    held = open_dem_files(s.proc.pid)
    if held is None:
        check("open files are observable", True, "neither /proc nor lsof")
    else:
        check("-n 1 keeps one DEM open", 1, len(held))
        for _ in range(10):
            s.post(NORTH)
            s.post(MIDDLE)
            s.post(SOUTH)
        check("still one after 30 lookups", 1, len(open_dem_files(s.proc.pid)))
        check("and the answers are still right", [1111], s.post(NORTH)[1])
    check_shutdown("open-file cap", s)

    # The control. Without it, "one file open" could just as well mean the
    # counter never sees anything.
    s = Server(binary, capdir, "-n", "0")
    s.post(NORTH)
    s.post(MIDDLE)
    s.post(SOUTH)
    held = open_dem_files(s.proc.pid)
    if held is not None:
        check("uncapped keeps all three open", 3, len(held))
    check_shutdown("uncapped", s)

    print("== lookup grid ==")
    # A raster far larger than the median dataset is not enumerated into every
    # cell it covers -- it goes on a list consulted alongside whatever the
    # cell holds. That list is where precedence can break silently: merged in
    # the wrong place it changes which dataset answers without changing the
    # set of datasets involved, and both orders look equally plausible in a
    # response.
    griddir = tempfile.mkdtemp(prefix="demd-test-grid-")
    names = ("N22E120.hgt", "N22E121.hgt", "N23E120.hgt", "N23E121.hgt")
    for name in names:
        mkdem(os.path.join(griddir, name))
    tiles = [os.path.join(griddir, n) for n in names]
    wide = os.path.join(griddir, "wide.tif")
    mktif(wide, "--rotated", "--value", "5000")

    wide_first = os.path.join(griddir, "wide-first.idx")
    tiles_first = os.path.join(griddir, "tiles-first.idx")
    check("index, wide raster first", 0, run_cli(binary, "-w", wide_first, wide, *tiles)[0])
    check("index, tiles first", 0, run_cli(binary, "-w", tiles_first, *tiles, wide)[0])

    s = Server(binary, wide_first)
    # Without this the two orders below prove nothing about the oversized
    # path: they would pass identically on a build that never used it.
    check("the wide raster is oversized for the grid", True,
          "1 oversized dataset(s)" in s.output())
    check("wide first: it answers over the tile", [5000], s.post("[[120.25,23.75]]")[1])
    # North of every tile, but well inside the rotated raster -- the bbox
    # corners of a rotated square are not covered by the square, so a point
    # has to be chosen inside the image, not merely inside its envelope.
    check("wide first: ground only it covers", [5000], s.post("[[121.5,25.03]]")[1])
    check("outside every dataset -> null", [None], s.post("[[0.0,0.0]]")[1])
    check_shutdown("grid, wide first", s)

    s = Server(binary, tiles_first)
    check("tiles first: the tile answers", [1000], s.post("[[120.25,23.75]]")[1])
    check("tiles first: ground only the wide raster covers", [5000],
          s.post("[[121.5,25.03]]")[1])
    check("tiles first: a neighbouring tile", [1000], s.post("[[121.25,22.75]]")[1])
    check_shutdown("grid, tiles first", s)

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
