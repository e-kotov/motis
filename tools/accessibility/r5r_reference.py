#!/usr/bin/env python3
"""Independent reference for `motis-accessibility route --r5r-out`.

    r5r_reference.py --stream S.bin --r5r-dir DIR --percentiles 5,50,95
                     [--opportunities cells.csv --opportunity-columns pop --cutoffs 30,60
                      --decay step|linear:W|exponential|fixed-exponential:K|logistic:SD]

Reads the per-departure travel-time stream of `route --out` (the same origins,
departures and options), recomputes the r5r-style outputs from it following the
Conveyal R5 source (analyst/TravelTimeReducer.java, analyst/decay/*.java) and
compares them with ttm.bin and accessibility.csv: travel-time percentiles must be
equal, accessibility values must be equal as doubles. Exit 0 = identical.
Standard library only.
"""
import argparse
import csv
import ctypes
import errno
import math
import os
import re
import struct
import sys

UNREACHED = 2**31 - 1
TWO_HOURS = 7200

# std::stod uses strtod, including its hexadecimal syntax and range errors.
_strtod = ctypes.CDLL(None, use_errno=True).strtod
_strtod.argtypes = (ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))
_strtod.restype = ctypes.c_double


def read_stream(path):
    with open(path, "rb") as f:
        hdr = f.read(56)
        if len(hdr) != 56:
            raise ValueError("truncated stream: header")
        magic = hdr[:8]
        if magic != b"VRTTMATX":
            raise ValueError("not a route stream")
        (version, n_total, n_dests, n_dep, o_from, o_to, cap, flags, target) = struct.unpack_from("<IIIIIIHBB", hdr, 8)
        if version != 1:
            raise ValueError(f"unsupported stream version {version}")
        if n_dep == 0:
            raise ValueError("stream n_departures must be at least 1")
        if len(f.read(8 * n_dep)) != 8 * n_dep:
            raise ValueError("truncated stream: departures")
        u16 = bool(flags & 4)
        size = 2 if u16 else 1
        unreach = 0xFFFF if u16 else 0xFF
        block = n_dep * n_dests
        extra = (block if flags & 1 else 0) + (4 * block if flags & 2 else 0)
        out = {}
        while True:
            b = f.read(4)
            if not b:
                break  # clean end: no bytes after the last complete record
            if len(b) != 4:
                raise ValueError(f"truncated stream: {len(b)} trailing byte(s), not a complete origin id")
            (o,) = struct.unpack("<I", b)
            if o in out:
                raise ValueError(f"duplicate stream record for origin {o}")
            raw = f.read(block * size)
            if len(raw) != block * size:
                raise ValueError("truncated stream: travel-time block")
            vals = struct.unpack(f"<{block}{'H' if u16 else 'B'}", raw)
            if len(f.read(extra)) != extra:  # transfers (u8) and egress-stop (u32) blocks
                raise ValueError("truncated stream: transfer / egress-stop block")
            out[o] = vals
    return n_dests, n_dep, unreach, out


def strictly_increasing(spec, lo, hi, name):
    """--percentiles / --cutoffs, as the C++ tool: strictly increasing unique
    integers in [lo, hi]."""
    v = []
    for x in spec.split(","):
        if re.fullmatch(r"\A-?[0-9]+\Z", x) is None:
            raise ValueError(f"bad number {x!r}")
        try:
            v.append(int(x))
        except ValueError:
            raise ValueError(f"bad number {x!r}") from None
    for i, x in enumerate(v):
        if not lo <= x <= hi:
            raise ValueError(f"--{name}: {x} is not an integer in [{lo}, {hi}]")
        if i and x <= v[i - 1]:
            raise ValueError(f"--{name} must be strictly increasing and unique, got {x} after {v[i - 1]}")
    return v


def parse_csv_line(line):
    """Parse one physical CSV line, with doubled quotes inside quoted fields."""
    row = []
    start = 0
    while True:
        if line[start:start + 1] == b'"':
            parts = []
            start += 1
            while True:
                quote = line.find(b'"', start)
                if quote == -1:
                    raise ValueError("unclosed quoted CSV field")
                parts.append(line[start:quote])
                start = quote + 1
                if line[start:start + 1] != b'"':
                    break
                parts.append(b'"')
                start += 1
            row.append(b"".join(parts))
            if start == len(line):
                return row
            if line[start:start + 1] != b",":
                raise ValueError("expected comma after quoted CSV field")
        else:
            comma = line.find(b",", start)
            field = line[start:] if comma == -1 else line[start:comma]
            if b'"' in field:
                raise ValueError("quote in unquoted CSV field")
            row.append(field)
            if comma == -1:
                return row
            start = comma
        start += 1


def opportunity_count(token):
    start = ctypes.create_string_buffer(token)
    end = ctypes.c_void_p()
    ctypes.set_errno(0)
    value = _strtod(start, ctypes.byref(end))
    if ctypes.get_errno() == errno.ERANGE:
        raise ValueError("stod")
    if not token or end.value != ctypes.addressof(start) + len(token):
        raise ValueError(f"bad number '{token.decode('utf-8', errors='backslashreplace')}'")
    if not math.isfinite(value) or value < 0:
        raise ValueError("opportunity counts must be finite and non-negative")
    return value


def read_opportunities(path, columns, n_dests):
    table = []
    try:
        # Split on LF and strip one terminal CR, as for std::getline.
        with open(path, "rb") as f:
            for line in f:
                line = line.removesuffix(b"\n").removesuffix(b"\r")
                if line:
                    table.append(parse_csv_line(line))
    except OSError:
        raise ValueError(f"cannot open {path}") from None
    header = table[0] if table else []
    rows = table[1:]
    if len(rows) != n_dests:
        raise ValueError(f"--opportunities: {len(rows)} rows, expected {n_dests} destinations")

    for i, row in enumerate(rows, 1):
        if len(row) != len(header):
            raise ValueError(f"--opportunities: row {i}: expected {len(header)} columns, got {len(row)}")

    opportunities = {}
    for name in columns.split(","):
        try:
            col = header.index(os.fsencode(name))
        except ValueError:
            raise ValueError(f"column {name} missing") from None
        opportunities[name] = [opportunity_count(row[col]) for row in rows]
    return opportunities


def percentile_index(n, p):
    """TravelTimeReducer.findPercentileIndex, ceil(p / 100 * n) - 1, for integer
    p in 1..100, in exact integer arithmetic (no float drift on boundaries)."""
    if not (isinstance(p, int) and 1 <= p <= 100):
        raise ValueError(f"percentile {p!r} is not an integer in 1..100")
    return -(-p * n // 100) - 1


def safe_exp(x):
    """math.exp, but +inf on overflow (as IEEE / C++ std::exp)."""
    try:
        return math.exp(x)
    except OverflowError:
        return math.inf


class Decay:
    def __init__(self, spec):
        name, _, param = spec.partition(":")
        self.name = name
        if name == "linear":
            self.width = int(param) * 60
            self.half = self.width // 2
            self.table = [1 - ((s + 0.5) / self.width) for s in range(self.width)]
        elif name == "fixed-exponential":
            self.k = float(param)
        elif name == "logistic":
            self.sd = float(param) * 60
        elif name not in ("step", "exponential"):
            raise ValueError(spec)

    def weight(self, c, t):
        n = self.name
        if n == "step":
            return 1 if t < c else 0
        if n == "linear":
            idx = t - (c - self.half)
            return 1 if idx < 0 else (self.table[idx] if idx < self.width else 0)
        if n in ("exponential", "fixed-exponential"):
            if t <= 0:
                return 1
            if t >= TWO_HOURS:
                return 0
            return safe_exp(math.log(0.5) / c * t) if n == "exponential" else safe_exp(-self.k * t)
        g = lambda m, tt: 1 + safe_exp(((tt - m) * math.pi) / (self.sd * math.sqrt(3)))
        return g(c, 0) / g(c, t)

    def zero(self, c):  # DecayFunction.reachesZeroAt
        if self.name == "step":
            return c
        if self.name == "linear":
            return c + self.half
        lo, hi = 0, 4 * 3600
        while lo < hi:
            mid = (lo + hi) // 2
            if self.weight(c, mid) < 0.001:
                hi = mid
            else:
                lo = mid + 1
        return lo


def selftest():
    """Hand-ranked boundaries: with times 1..N, percentile p is the value at
    rank k = the smallest k with 100 * k >= p * N (counted by a loop here)."""
    bad = 0
    for n in (1, 2, 3, 7, 30, 99, 100, 101, 120, 150, 151, 1440):
        for p in range(1, 101):
            k = 1
            while 100 * k < p * n:
                k += 1
            if percentile_index(n, p) + 1 != k:
                bad += 1
                print(f"N={n} p={p}: index {percentile_index(n, p)} rank {k}")
    # the float formula this replaces drifts, e.g. N=100, p=7 -> rank 8
    drift = [p for p in range(1, 101) if int(math.ceil(p / 100 * 100) - 1) + 1 != p]
    print(f"selftest: failures={bad}; float formula would misrank N=100 at p={drift}")
    for spec in ("logistic:0.1", "logistic:10", "exponential", "fixed-exponential:0.001", "linear:10", "step"):
        d = Decay(spec)
        for c in (60, 1800, 3600):
            for t in (0, 1, 59, 60, 61, 1800, 7199, 7200, 14400, 86400):
                w = d.weight(c, t)
                if not (0 <= w <= 1):
                    bad += 1
                    print(f"{spec} weight({c}, {t}) = {w}")
            d.zero(c)
    print(f"selftest: {'OK' if bad == 0 else 'FAILED'}")
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream")
    ap.add_argument("--r5r-dir")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--percentiles", default="50")
    ap.add_argument("--opportunities")
    ap.add_argument("--opportunity-columns")
    ap.add_argument("--cutoffs")
    ap.add_argument("--decay", default="step")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    pcts = strictly_increasing(a.percentiles, 1, 100, "percentiles")
    if a.opportunities is None:
        if a.cutoffs is not None:
            raise ValueError("unknown option --cutoffs")
        if a.opportunity_columns is not None:
            raise ValueError("unknown option --opportunity-columns")
    elif a.cutoffs is None:
        raise ValueError("missing --cutoffs")
    cutoffs = strictly_increasing(a.cutoffs, 1, 240, "cutoffs") if a.cutoffs is not None else []
    if a.opportunities is not None and not a.opportunity_columns:
        raise ValueError("missing --opportunity-columns")
    n_dests, n_dep, unreach, stream = read_stream(a.stream)
    idx = [percentile_index(n_dep, p) for p in pcts]
    opp = {}
    if a.opportunities is not None:
        opp = read_opportunities(a.opportunities, a.opportunity_columns, n_dests)
    decay = Decay(a.decay)
    zeros = [decay.zero(c * 60) for c in cutoffs]

    expected_ttm, expected_acc = {}, {}
    for o, vals in stream.items():
        pt_all = []
        for d in range(n_dests):
            ts = sorted(UNREACHED if vals[di * n_dests + d] == unreach else vals[di * n_dests + d] * 60
                        for di in range(n_dep))
            pt_all.append([ts[i] for i in idx])
        expected_ttm[o] = [0xFFFF if t == UNREACHED else t // 60 for pt in pt_all for t in pt]
        for name, counts in opp.items():
            acc = [[0.0] * len(cutoffs) for _ in pcts]
            for d in range(n_dests):
                if not counts[d] > 0:
                    continue
                for pi, t in enumerate(pt_all[d]):
                    if t == UNREACHED:
                        continue  # this percentile only; the others are independent
                    for ci in range(len(cutoffs) - 1, -1, -1):
                        if t >= zeros[ci]:
                            break
                        w = decay.weight(cutoffs[ci] * 60, t)
                        if w > 0:
                            acc[pi][ci] += counts[d] * w
            for pi, p in enumerate(pcts):
                for ci, c in enumerate(cutoffs):
                    expected_acc[(o, name, p, c)] = acc[pi][ci]

    # candidate outputs
    ok = True
    with open(f"{a.r5r_dir}/ttm.bin", "rb") as f:
        magic = f.read(8)
        version, nd, npct, ndep = struct.unpack("<IIII", f.read(16))
        got_p = struct.unpack(f"<{npct}d", f.read(8 * npct))
        if magic != b"VRR5TTM1" or version != 1 or nd != n_dests or ndep != n_dep or list(got_p) != [float(p) for p in pcts]:
            print(f"ttm.bin header mismatch (magic {magic!r}, version {version})"); ok = False
        got = {}
        while True:
            b = f.read(4)
            if not b:
                break
            if len(b) != 4:
                print(f"ttm.bin: {len(b)} trailing byte(s)"); ok = False; break
            (o,) = struct.unpack("<I", b)
            if o in got:
                print(f"ttm.bin: duplicate record for origin {o}"); ok = False
            rec = f.read(2 * nd * npct)
            if len(rec) != 2 * nd * npct:
                print("ttm.bin: truncated record"); ok = False; break
            got[o] = list(struct.unpack(f"<{nd * npct}H", rec))
    if set(got) != set(expected_ttm):
        print("ttm.bin origin set differs"); ok = False
    diff = sum(1 for o in expected_ttm if got.get(o) != expected_ttm[o])
    cells = sum(len(v) for v in expected_ttm.values())
    reach = sum(1 for v in expected_ttm.values() for x in v if x != 0xFFFF)
    print(f"ttm: origins={len(expected_ttm)} values={cells} reached={reach} origins_differing={diff}")
    ok &= diff == 0
    got_acc = {}
    for r in csv.DictReader(open(f"{a.r5r_dir}/accessibility.csv")):
        k = (int(r["origin"]), r["opportunity"], int(r["percentile"]), int(r["cutoff"]))
        if k in got_acc:
            print(f"accessibility.csv: duplicate row {k}"); ok = False
        got_acc[k] = float(r["accessibility"])
    if set(got_acc) != set(expected_acc):
        print(f"accessibility keys differ: got {len(got_acc)} expected {len(expected_acc)}"); ok = False
    nd_acc = [k for k in expected_acc if got_acc.get(k) != expected_acc[k]]
    nz = sum(1 for v in expected_acc.values() if v > 0)
    print(f"accessibility: values={len(expected_acc)} non_zero={nz} differing={len(nd_acc)}")
    for k in nd_acc[:5]:
        print("  differs", k, got_acc.get(k), expected_acc[k])
    ok &= not nd_acc
    print("r5r_reference: IDENTICAL" if ok else "r5r_reference: DIFFERENT")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except ValueError as e:  # invalid options or input files
        print(f"invalid: {e}", file=sys.stderr)
        sys.exit(2)
