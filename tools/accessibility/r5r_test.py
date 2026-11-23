#!/usr/bin/env python3
"""Regression tests for the r5r-style outputs, on a synthetic route stream.

    r5r_test.py TMP_DIR [MOTIS_ACCESSIBILITY]

Always (r5r_reference.py): the valid stream parses; a stream with its final
byte (in the transfers block) removed, and one with a trailing partial origin
id, are rejected (exit 2); --percentiles 100,7 and 7,7 are rejected (exit 2);
--opportunities with --cutoffs omitted or empty ("") is rejected (exit 2), checked
against a hand-written travel-time-only output (p50) that the reference without
--opportunities reports IDENTICAL, so a false IDENTICAL cannot pass.
With the binary also `r5r-reduce`: on the valid stream its outputs equal the
reference (IDENTICAL), it rejects the same six inputs (exit 2), and the same
cutoff cases are rejected against its own travel-time-only output.
Exit 0 if every expectation holds. Standard library only.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "r5r_reference.py")
sys.path.insert(0, HERE)
import r5r_reference  # noqa: E402

N_DESTS, N_DEP = 3, 4
U8_UNREACHED = 255
# per origin: travel minutes [departure][destination], transfers [departure][destination]
RECORDS = {
    0: ([[10, 40, U8_UNREACHED], [12, 41, U8_UNREACHED], [11, U8_UNREACHED, 70], [9, 35, U8_UNREACHED]],
        [[0, 1, 255], [0, 1, 255], [0, 255, 2], [0, 1, 255]]),
    1: ([[U8_UNREACHED, 20, 30], [U8_UNREACHED, 22, 31], [50, 25, 33], [U8_UNREACHED, 21, 29]],
        [[255, 0, 1], [255, 0, 1], [2, 0, 1], [255, 0, 1]]),
}
OPTS = ["--percentiles", "5,50,100", "--cutoffs", "15,30,45", "--decay", "step"]
# p50 over the 4 departures (rank ceil(0.5 * 4) = 2nd smallest, unreached last), by hand
TTM_P50 = {0: [10, 40, 0xFFFF], 1: [0xFFFF, 21, 30]}


def write_stream(path):
    hdr = b"VRTTMATX" + struct.pack("<IIIIIIHBB", 1, 2, N_DESTS, N_DEP, 0, 2, 95, 1, 0) + bytes(20)
    assert len(hdr) == 56
    body = b"".join(struct.pack("<q", 29927820 + i) for i in range(N_DEP))
    for o, (tt, tr) in RECORDS.items():
        body += struct.pack("<I", o) + bytes(x for row in tt for x in row) + bytes(x for row in tr for x in row)
    open(path, "wb").write(hdr + body)


def write_ttm_only(out_dir):
    """Travel-time-only r5r output (--percentiles 50, no --opportunities)."""
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "ttm.bin"), "wb") as f:
        f.write(b"VRR5TTM1" + struct.pack("<IIII", 1, N_DESTS, 1, N_DEP) + struct.pack("<d", 50.0))
        for o, vals in TTM_P50.items():
            f.write(struct.pack("<I", o) + struct.pack(f"<{N_DESTS}H", *vals))
    open(os.path.join(out_dir, "accessibility.csv"), "w").write("origin,opportunity,percentile,cutoff,accessibility\n")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    tmp = sys.argv[1]
    binary = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(tmp, exist_ok=True)
    ok = True

    def expect(label, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"{'OK  ' if cond else 'FAIL'} {label} {detail}".rstrip())

    valid = os.path.join(tmp, "valid.bin")
    write_stream(valid)
    opp = os.path.join(tmp, "opp.csv")
    open(opp, "w").write("pop\n10\n0\n5\n")
    cut = os.path.join(tmp, "cut.bin")      # final byte (last transfer code) removed
    open(cut, "wb").write(open(valid, "rb").read()[:-1])
    plus = os.path.join(tmp, "plus.bin")    # one trailing byte (partial origin id)
    open(plus, "wb").write(open(valid, "rb").read() + b"\x01")

    n_dests, n_dep, _, recs = r5r_reference.read_stream(valid)
    expect("reference parses the valid stream", (n_dests, n_dep, sorted(recs)) == (N_DESTS, N_DEP, [0, 1]))

    # reference CLI: invalid inputs fail before any output is read (exit 2)
    dummy = os.path.join(tmp, "no-outputs")
    base = [sys.executable, REF, "--r5r-dir", dummy, "--opportunities", opp, "--opportunity-columns", "pop"]
    negative = [
        ("stream with final transfer byte removed", ["--stream", cut] + OPTS, "transfer / egress-stop block"),
        ("stream with 1 trailing byte", ["--stream", plus] + OPTS, "trailing byte"),
        ("--percentiles 100,7", ["--stream", valid, "--percentiles", "100,7", "--cutoffs", "15,30,45"],
         "strictly increasing"),
        ("--percentiles 7,7", ["--stream", valid, "--percentiles", "7,7", "--cutoffs", "15,30,45"],
         "strictly increasing"),
    ]
    for label, args, reason in negative:
        rc, out = run(base + args)
        expect(f"reference rejects {label}", rc == 2 and reason in out, f"(rc={rc}: {out[-160:]})")

    # --opportunities needs a non-empty --cutoffs list, as in the C++ tool. Checked against an
    # existing travel-time-only output, where an ignored accessibility part would read IDENTICAL.
    def cutoff_cases(tt_dir, tool):
        rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", tt_dir, "--percentiles", "50"])
        expect(f"reference: {tool} travel-time-only output (no --opportunities) is IDENTICAL",
               rc == 0 and "IDENTICAL" in out, f"(rc={rc}: {out[-200:]})")
        for label, extra, reason in [("omitted", [], "missing --cutoffs"), ('""', ["--cutoffs", ""], "bad number ''")]:
            rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", tt_dir, "--percentiles", "50",
                           "--opportunities", opp, "--opportunity-columns", "pop"] + extra)
            expect(f"reference rejects --opportunities with --cutoffs {label} ({tool} output)",
                   rc == 2 and reason in out, f"(rc={rc}: {out[-160:]})")

    tt_hand = os.path.join(tmp, "tt-hand")
    write_ttm_only(tt_hand)
    cutoff_cases(tt_hand, "hand-written")
    negative += [
        ("--opportunities with --cutoffs omitted", ["--stream", valid, "--percentiles", "50"], "missing --cutoffs"),
        ('--opportunities with --cutoffs ""', ["--stream", valid, "--percentiles", "50", "--cutoffs", ""],
         "bad number ''"),
    ]

    if binary:
        out_dir = os.path.join(tmp, "r5r")
        rc, out = run([binary, "r5r-reduce", "--stream", valid, "--r5r-out", out_dir, "--opportunities", opp,
                       "--opportunity-columns", "pop"] + OPTS)
        expect("r5r-reduce accepts the valid stream", rc == 0, f"(rc={rc}: {out[-160:]})")
        rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", out_dir, "--opportunities", opp,
                       "--opportunity-columns", "pop"] + OPTS)
        expect("r5r-reduce outputs == reference", rc == 0 and "IDENTICAL" in out, f"(rc={rc}: {out[-200:]})")
        tt_bin = os.path.join(tmp, "tt-bin")
        rc, out = run([binary, "r5r-reduce", "--stream", valid, "--r5r-out", tt_bin, "--percentiles", "50"])
        expect("r5r-reduce writes a travel-time-only output", rc == 0, f"(rc={rc}: {out[-160:]})")
        cutoff_cases(tt_bin, "r5r-reduce")
        for label, args, reason in negative:
            stream = args[1]
            opts = args[2:] if len(args) > 2 else []
            cmd = [binary, "r5r-reduce", "--stream", stream, "--r5r-out", os.path.join(tmp, "neg"),
                   "--opportunities", opp, "--opportunity-columns", "pop"] + (opts or OPTS)
            rc, out = run(cmd)
            expect(f"r5r-reduce rejects {label}", rc == 2 and reason in out, f"(rc={rc}: {out[-160:]})")
    print("r5r_test: OK" if ok else "r5r_test: FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
