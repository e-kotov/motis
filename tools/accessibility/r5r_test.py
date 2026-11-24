#!/usr/bin/env python3
"""Regression tests for the r5r-style outputs, on a synthetic route stream.

    r5r_test.py TMP_DIR [--bin MOTIS_ACCESSIBILITY] [--route-data RULES_MIDNIGHT_DATA]

Always (r5r_reference.py): the valid stream parses; a stream with its final
byte (in the transfers block) removed, and one with a trailing partial origin
id, are rejected (exit 2); --percentiles 100,7 and 7,7 are rejected (exit 2);
--opportunities with --cutoffs omitted or empty ("") is rejected (exit 2), even
when --opportunities itself is empty. An empty path with valid cutoffs also fails.
These cases use a hand-written travel-time-only output (p50) that the reference
without --opportunities reports IDENTICAL, so a false IDENTICAL cannot pass.
With the binary also `r5r-reduce`: on the valid stream its outputs equal the
reference (IDENTICAL), it rejects the same invalid inputs (exit 2), and the same
cutoff cases are rejected against its own travel-time-only output.
Also exercises valid and malformed opportunity CSVs, duplicate origins, strict integer tokens,
zero departures and accessibility option dependencies. With a retained
rules-midnight import, repeat the CSV cases through route using hand-written
direct connectors.
Exit 0 if every expectation holds. Standard library only.
"""
import argparse
import csv
import os
import struct
import subprocess
import sys
from pathlib import Path

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


def write_stream(path, records=RECORDS, n_dests=N_DESTS, n_dep=N_DEP):
    hdr = b"VRTTMATX" + struct.pack("<IIIIIIHBB", 1, 2, n_dests, n_dep, 0, 2, 95, 1, 0) + bytes(20)
    assert len(hdr) == 56
    body = b"".join(struct.pack("<q", 29927820 + i) for i in range(n_dep))
    for o, (tt, tr) in records.items():
        body += struct.pack("<I", o) + bytes(x for row in tt for x in row) + bytes(x for row in tr for x in row)
    open(path, "wb").write(hdr + body)


def write_ttm_only(out_dir, percentiles=(50,), records=TTM_P50, n_dests=N_DESTS, n_dep=N_DEP):
    """Write hand-calculated percentile records for the reference CLI."""
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "ttm.bin"), "wb") as f:
        n_pcts = len(percentiles)
        f.write(b"VRR5TTM1" + struct.pack("<IIII", 1, n_dests, n_pcts, n_dep))
        f.write(struct.pack(f"<{n_pcts}d", *percentiles))
        for o, vals in records.items():
            f.write(struct.pack("<I", o) + struct.pack(f"<{n_dests * n_pcts}H", *vals))
    open(os.path.join(out_dir, "accessibility.csv"), "w").write("origin,opportunity,percentile,cutoff,accessibility\n")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def stage3_cases(tmp, binary, route_data, valid, opp, tt_hand, expect):
    work = Path(tmp)
    valid_stream = Path(valid).read_bytes()
    duplicate = work / "duplicate.bin"
    duplicate.write_bytes(valid_stream + valid_stream[56 + 8 * N_DEP:][:4 + 2 * N_DEP * N_DESTS])
    empty = work / "n-dep-zero.bin"
    write_stream(empty, {0: ([], [])}, n_dep=0)

    # Rank 1 of 4, with unreachable values last, followed by the hand-ranked p50.
    p5_p50 = {0: [9, 10, 35, 40, 70, 0xFFFF], 1: [50, 0xFFFF, 20, 21, 29, 30]}
    p_first = {o: vals[::2] for o, vals in p5_p50.items()}
    negatives = [
        ("duplicate stream record", duplicate, tt_hand, [], "duplicate stream record for origin 0"),
        ("n_departures=0", empty, tt_hand, [], "stream n_departures must be at least 1"),
        ("--cutoffs without opportunities", valid, tt_hand, ["--cutoffs", "30"], "unknown option --cutoffs"),
        ("--opportunity-columns without opportunities", valid, tt_hand,
         ["--opportunity-columns", "pop"], "unknown option --opportunity-columns"),
        ("--opportunities without columns", valid, tt_hand,
         ["--opportunities", opp, "--cutoffs", "30"], "missing --opportunity-columns"),
        ("--opportunities with empty columns", valid, tt_hand,
         ["--opportunities", opp, "--cutoffs", "30", "--opportunity-columns", ""], "missing --opportunity-columns"),
    ]
    for i, (token, pcts, records, bad) in enumerate((
        ("5, 50", (5, 50), p5_p50, " 50"),
        ("+7", (7,), p_first, "+7"),
        (" 7", (7,), p_first, " 7"),
        ("٧", (7,), p_first, "٧"),
        ("1_0", (10,), p_first, "1_0"),
    )):
        hand = work / f"tt-int-{i}"
        write_ttm_only(hand, pcts, records)
        negatives.append((f"--percentiles {token!r}", valid, hand, ["--percentiles", token], f"bad number '{bad}'"))

    # Each origin can reach only its matching destination in 5 minutes.
    quoted_stream = work / "quoted.bin"
    write_stream(quoted_stream,
                 {0: ([[5, U8_UNREACHED]], [[254, 255]]), 1: ([[U8_UNREACHED, 5]], [[255, 254]])},
                 n_dests=2, n_dep=1)
    quoted = work / "quoted.csv"
    quoted.write_text('label,pop\n"district,99",10\n"district,98",0\n', encoding="utf-8")

    def csv_fixture(name, data):
        path = work / f"{name}.csv"
        path.write_bytes(data)
        return path

    def token_row(token):
        return b"label,pop\nfirst,10\nsecond," + token + b"\n"

    valid_csv = [
        (quoted, 0),
        (csv_fixture("quoted-crlf", b'\r\n"label","pop"\r\n"district ""A"",99","+1e1"\r\n'
                                   b'\n"district ""B"",98",-0.0\r\n'), 0),
        (csv_fixture("duplicate-header", b"label,pop,pop\na,10,450\nb,0,120\n"), 0),
        (csv_fixture("empty-unused-header", b",pop\na,10\nb,0\n"), 0),
    ]
    for name, token, value in (
        ("leading-plus", b"+10", 10),
        ("scientific", b"1e2", 100),
        ("negative-exponent", b"1.5e-2", 0.015),
        ("leading-dot", b".5", 0.5),
        ("trailing-dot", b"5.", 5),
        ("signed-exponent", b"+.5E+1", 5),
        ("negative-zero", b"-0.0", 0),
    ):
        valid_csv.append((csv_fixture(name, token_row(token)), value))

    malformed = [
        ("unclosed", b'label,pop\n"district,99,10\ndistrict,0\n', "unclosed quoted CSV field"),
        ("unclosed-last", b'label,pop\n"district,99",10\n"district,98","0\n', "unclosed quoted CSV field"),
        ("unclosed-last-no-lf", b'label,pop\n"district,99",10\n"district,98","0', "unclosed quoted CSV field"),
        ("unescaped-quote", b'label,pop\ndistr"ict,10\ndistrict,0\n', "quote in unquoted CSV field"),
        ("after-closing-quote", b'label,pop\n"district"x,10\ndistrict,0\n', "expected comma after quoted CSV field"),
        ("space-before-quote", token_row(b' "0"'), "quote in unquoted CSV field"),
        ("space-after-quote", token_row(b'"0" '), "expected comma after quoted CSV field"),
        ("trailing-quote", token_row(b'"99"""'), "bad number '99\"'"),
        ("quoted-leading-space", token_row(b'" 10"'), "bad number ' 10'"),
        ("quoted-trailing-space", token_row(b'"10 "'), "bad number '10 '"),
        ("overflow", token_row(b"1e999"), "opportunity count out of range '1e999'"),
        ("underflow", token_row(b"1e-999"), "opportunity count out of range '1e-999'"),
        ("empty-file", b"", "--opportunities: 0 rows, expected 2 destinations"),
        ("blank-file", b"\r\n\n", "--opportunities: 0 rows, expected 2 destinations"),
        ("spaces-only-line", token_row(b"0") + b"  \n", "--opportunities: 3 rows, expected 2 destinations"),
        ("malformed-header", b'la"bel,pop\ndistrict,10\ndistrict,0\n', "quote in unquoted CSV field"),
        ("missing-column", b"label,other\ndistrict,10\ndistrict,0\n", "column pop missing"),
        ("empty-selected-header", b"label,\ndistrict,10\ndistrict,0\n", "column pop missing"),
        ("row-count", b"label,pop\ndistrict,10\n", "--opportunities: 1 rows, expected 2 destinations"),
        ("short-row", b"label,pop\ndistrict,10\ndistrict\n", "--opportunities: row 2: expected 2 columns, got 1"),
        ("short-row-unused", b"label,pop,ignored\na,10,x\nb,0\n", "--opportunities: row 2: expected 3 columns, got 2"),
        ("long-row", token_row(b"0,extra"), "--opportunities: row 2: expected 2 columns, got 3"),
        ("negative", token_row(b"-1"), "opportunity counts must be finite and non-negative"),
    ]
    for name, token in (
        ("nan", "NaN"), ("inf", "inf"), ("positive-inf", "+Inf"), ("negative-inf", "-Inf"),
        ("infinity", "Infinity"), ("underscores", "1_0"), ("hex", "0x1p2"), ("trailing-number", "10oops"),
        ("leading-space", " 10"), ("trailing-space", "10 "), ("leading-tab", "\t10"), ("trailing-tab", "10\t"),
        ("empty-count", ""), ("spaces-count", "   "), ("no-number", "oops"), ("missing-exponent", "1e+"),
        ("missing-mantissa", ".e1"), ("unicode-digit", "٧"),
    ):
        malformed.append((name, token_row(token.encode()), f"bad number '{token}'"))
    invalid_csv = [(csv_fixture(name, data), reason) for name, data, reason in malformed]

    def hand_output(path, value):
        out_dir = work / f"tt-{path.stem}"
        write_ttm_only(out_dir, records={0: [5, 0xFFFF], 1: [0xFFFF, 5]}, n_dests=2, n_dep=1)
        (out_dir / "accessibility.csv").write_text(
            "origin,opportunity,percentile,cutoff,accessibility\n0,pop,50,30,10\n" f"1,pop,50,30,{value}\n",
            encoding="utf-8",
        )
        return out_dir

    hand = hand_output(quoted, 0)

    def options(path, cutoff="30"):
        return ["--opportunities", path, "--opportunity-columns", "pop", "--cutoffs", cutoff]

    negatives.append(("--cutoffs '+30'", quoted_stream, hand, options(quoted, "+30"), "bad number '+30'"))
    for label, stream, out_dir, extra, reason in negatives:
        rc, out = run([sys.executable, REF, "--stream", stream, "--r5r-dir", out_dir] + extra)
        expect(f"reference rejects {label}", rc == 2 and out == f"invalid: {reason}", f"(rc={rc}: {out[-160:]})")
        if binary:
            rc, out = run([binary, "r5r-reduce", "--stream", stream, "--r5r-out", work / "neg-stage3"] + extra)
            expect(f"r5r-reduce rejects {label}", rc == 2 and out == f"error: {reason}", f"(rc={rc}: {out[-160:]})")

    def check_reference(label, stream, path, out_dir):
        rc, out = run([sys.executable, REF, "--stream", stream, "--r5r-dir", out_dir] + options(path))
        expect(f"{label} agrees with the reference", rc == 0 and "IDENTICAL" in out, f"(rc={rc}: {out[-240:]})")

    def check_compiled(mode, base, stream):
        for path, value in valid_csv:
            out_dir = work / f"{mode}-{path.stem}"
            rc, out = run(base + ["--r5r-out", out_dir] + options(path))
            expect(f"{mode} accepts {path.name}", rc == 0, f"(rc={rc}: {out[-160:]})")
            if rc == 0:
                with (out_dir / "accessibility.csv").open(newline="") as f:
                    totals = sorted((int(row["origin"]), float(row["accessibility"])) for row in csv.DictReader(f))
                expect(f"{mode} {path.name} totals", totals == [(0, 10), (1, value)], str(totals))
                check_reference(f"{mode} {path.name}", stream, path, out_dir)
        for path, reason in invalid_csv:
            rc, out = run(base + ["--r5r-out", work / f"{mode}-bad-csv"] + options(path))
            expect(f"{mode} rejects {path.name}",
                   rc == 2 and out.rpartition("\n")[2] == f"error: {reason}", f"(rc={rc}: {out[-160:]})")

    for path, value in valid_csv:
        check_reference(f"hand-written {path.name} output", quoted_stream, path, hand_output(path, value))
    for path, reason in invalid_csv:
        rc, out = run([sys.executable, REF, "--stream", quoted_stream, "--r5r-dir", hand] + options(path))
        expect(f"reference rejects {path.name}",
               rc == 2 and out == f"invalid: {reason}", f"(rc={rc}: {out[-240:]})")
    if binary:
        check_compiled("r5r-reduce", [binary, "r5r-reduce", "--stream", quoted_stream], quoted_stream)
    if route_data:
        connectors = work / "empty-connectors.csv"
        connectors.write_text("point,stop,minutes\n", encoding="utf-8")
        direct = work / "direct.csv"
        direct.write_text("point,stop,minutes\n0,0,5\n1,1,5\n", encoding="utf-8")
        route = [binary, "route", "--data", route_data, "--n-origins", "2", "--n-dests", "2",
                 "--dep-start-unix-minutes", "29927990", "--max-travel-minutes", "95", "--threads", "1",
                 "--access", connectors, "--egress", connectors, "--direct", direct]
        stream = work / "route-quoted.bin"
        rc, out = run(route + ["--out", stream])
        expect("route produces the quoted CSV case raw stream", rc == 0, f"(rc={rc}: {out[-160:]})")
        if rc == 0:
            check_reference("hand-written output for the raw route stream", stream, quoted, hand)
            check_compiled("route", route, stream)
        rc, out = run(route + ["--origin-list", "0,0", "--out", work / "duplicate-origin-list.bin"])
        expect("route rejects --origin-list 0,0", rc == 2 and out.rpartition("\n")[2] == "error: duplicate origin-list entry 0",
               f"(rc={rc}: {out[-160:]})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tmp")
    binaries = parser.add_mutually_exclusive_group()
    binaries.add_argument("binary", nargs="?")
    binaries.add_argument("--bin")
    parser.add_argument("--route-data", help="data directory of the imported rules-midnight fixture")
    cli = parser.parse_args()
    tmp, binary = cli.tmp, cli.bin or cli.binary
    if cli.route_data and not binary:
        parser.error("--route-data requires MOTIS_ACCESSIBILITY")
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
    cutoff_errors = [
        ("--opportunities with --cutoffs omitted", opp, [], "missing --cutoffs"),
        ('--opportunities with --cutoffs ""', opp, ["--cutoffs", ""], "bad number ''"),
        ('--opportunities "" with --cutoffs omitted', "", [], "missing --cutoffs"),
        ('--opportunities "" with --cutoffs ""', "", ["--cutoffs", ""], "bad number ''"),
        ('--opportunities "" with valid --cutoffs', "", ["--cutoffs", "15"], "cannot open"),
    ]

    def cutoff_cases(tt_dir, tool):
        rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", tt_dir, "--percentiles", "50"])
        expect(f"reference: {tool} travel-time-only output (no --opportunities) is IDENTICAL",
               rc == 0 and "IDENTICAL" in out, f"(rc={rc}: {out[-200:]})")
        for label, path, extra, reason in cutoff_errors:
            rc, out = run([sys.executable, REF, "--stream", valid, "--r5r-dir", tt_dir, "--percentiles", "50",
                           "--opportunities", path, "--opportunity-columns", "pop"] + extra)
            expect(f"reference rejects {label} ({tool} output)",
                   rc == 2 and reason in out, f"(rc={rc}: {out[-160:]})")

    tt_hand = os.path.join(tmp, "tt-hand")
    write_ttm_only(tt_hand)
    cutoff_cases(tt_hand, "hand-written")

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
            cmd = [binary, "r5r-reduce", "--r5r-out", os.path.join(tmp, "neg"),
                   "--opportunities", opp, "--opportunity-columns", "pop"] + args
            rc, out = run(cmd)
            expect(f"r5r-reduce rejects {label}", rc == 2 and reason in out, f"(rc={rc}: {out[-160:]})")
        for label, path, extra, reason in cutoff_errors:
            cmd = [binary, "r5r-reduce", "--stream", valid, "--r5r-out", tt_bin, "--percentiles", "50",
                   "--opportunities", path, "--opportunity-columns", "pop"] + extra
            rc, out = run(cmd)
            expect(f"r5r-reduce rejects {label}", rc == 2 and out == f"error: {reason}", f"(rc={rc}: {out[-160:]})")
    stage3_cases(tmp, binary, cli.route_data, valid, opp, tt_hand, expect)
    print("r5r_test: OK" if ok else "r5r_test: FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
