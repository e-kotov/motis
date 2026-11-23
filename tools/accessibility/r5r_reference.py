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
import math
import struct
import sys

UNREACHED = 2**31 - 1
TWO_HOURS = 7200


def read_stream(path):
    with open(path, "rb") as f:
        hdr = f.read(56)
        magic = hdr[:8]
        if magic != b"VRTTMATX":
            raise ValueError("not a route stream")
        (version, n_total, n_dests, n_dep, o_from, o_to, cap, flags, target) = struct.unpack_from("<IIIIIIHBB", hdr, 8)
        f.read(8 * n_dep)
        u16 = bool(flags & 4)
        size = 2 if u16 else 1
        unreach = 0xFFFF if u16 else 0xFF
        block = n_dep * n_dests
        extra = (block if flags & 1 else 0) + (4 * block if flags & 2 else 0)
        out = {}
        while True:
            b = f.read(4)
            if not b:
                break
            (o,) = struct.unpack("<I", b)
            raw = f.read(block * size)
            vals = struct.unpack(f"<{block}{'H' if u16 else 'B'}", raw)
            f.read(extra)
            out[o] = vals
    return n_dests, n_dep, unreach, out


def percentile_index(n, p):  # TravelTimeReducer.findPercentileIndex
    return int(math.ceil(p / 100 * n) - 1)


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
            return math.exp(math.log(0.5) / c * t) if n == "exponential" else math.exp(-self.k * t)
        g = lambda m, tt: 1 + math.exp(((tt - m) * math.pi) / (self.sd * math.sqrt(3)))
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream", required=True)
    ap.add_argument("--r5r-dir", required=True)
    ap.add_argument("--percentiles", default="50")
    ap.add_argument("--opportunities")
    ap.add_argument("--opportunity-columns", default="")
    ap.add_argument("--cutoffs", default="")
    ap.add_argument("--decay", default="step")
    a = ap.parse_args()
    pcts = [float(x) for x in a.percentiles.split(",")]
    n_dests, n_dep, unreach, stream = read_stream(a.stream)
    idx = [percentile_index(n_dep, p) for p in pcts]
    opp = {}
    if a.opportunities:
        rows = list(csv.DictReader(open(a.opportunities)))
        for c in a.opportunity_columns.split(","):
            opp[c] = [float(r[c]) for r in rows]
    cutoffs = [int(x) for x in a.cutoffs.split(",")] if a.cutoffs else []
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
                        break
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
        if magic != b"VRR5TTM1" or nd != n_dests or ndep != n_dep or list(got_p) != pcts:
            print("ttm.bin header mismatch"); ok = False
        got = {}
        while True:
            b = f.read(4)
            if not b:
                break
            (o,) = struct.unpack("<I", b)
            got[o] = list(struct.unpack(f"<{nd * npct}H", f.read(2 * nd * npct)))
    if set(got) != set(expected_ttm):
        print("ttm.bin origin set differs"); ok = False
    diff = sum(1 for o in expected_ttm if got.get(o) != expected_ttm[o])
    cells = sum(len(v) for v in expected_ttm.values())
    reach = sum(1 for v in expected_ttm.values() for x in v if x != 0xFFFF)
    print(f"ttm: origins={len(expected_ttm)} values={cells} reached={reach} origins_differing={diff}")
    ok &= diff == 0
    got_acc = {}
    for r in csv.DictReader(open(f"{a.r5r_dir}/accessibility.csv")):
        got_acc[(int(r["origin"]), r["opportunity"], float(r["percentile"]), int(r["cutoff"]))] = float(r["accessibility"])
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
    main()
