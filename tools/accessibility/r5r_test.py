#!/usr/bin/env python3
"""Regression tests for the r5r-style outputs, on a synthetic route stream.

    r5r_test.py TMP_DIR [MOTIS_ACCESSIBILITY]

Always (r5r_reference.py): the valid stream parses; a stream with its final
byte (in the transfers block) removed, and one with a trailing partial origin
id, are rejected (exit 2); --percentiles 100,7 and 7,7 are rejected (exit 2).
With the binary also `r5r-reduce`: on the valid stream its outputs equal the
reference (IDENTICAL), and it rejects the same four inputs (exit 2).
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


def write_stream(path):
    hdr = b"VRTTMATX" + struct.pack("<IIIIIIHBB", 1, 2, N_DESTS, N_DEP, 0, 2, 95, 1, 0) + bytes(20)
    assert len(hdr) == 56
    body = b"".join(struct.pack("<q", 29927820 + i) for i in range(N_DEP))
    for o, (tt, tr) in RECORDS.items():
        body += struct.pack("<I", o) + bytes(x for row in tt for x in row) + bytes(x for row in tr for x in row)
    open(path, "wb").write(hdr + body)


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

    if binary:
        out_dir = os.path.join(tmp, "r5r")
        rc, out = run([binary, "r5r-reduce", "--stream", valid, "--r5r-out", out_dir, "--opportunities", opp,
                       "--opportunity-columns", "pop"] + OPTS)
        expect("r5r-reduce accepts the valid stream", rc == 0, f"(rc={rc}: {out[-160:]})")
        rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", out_dir, "--opportunities", opp,
                       "--opportunity-columns", "pop"] + OPTS)
        expect("r5r-reduce outputs == reference", rc == 0 and "IDENTICAL" in out, f"(rc={rc}: {out[-200:]})")
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
