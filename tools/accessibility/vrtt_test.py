#!/usr/bin/env python3
"""Negative tests for `vrtt.py check`.

    vrtt_test.py VALID.vrtt [TMP_DIR]
    vrtt_test.py --synthetic [TMP_DIR]   (writes a small valid export with vrtt.write first)

The valid file must pass. Each corrupted copy must be rejected (exit 2) with
its own expected reason, each corruption isolated so that no earlier check
fires: duplicate section (replacing a required one), unknown section, missing
section (file re-laid out without it), reversed export interval, mismatched CSR
index array, non-monotone index array (final entry intact), trips outside the
export window, invalid UTF-8 in a location ID and a flipped payload byte. Except for the last case the checksum is recomputed (or the file rewritten)
so only the structural checks can catch them. Exit 0 if all expectations hold.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vrtt  # noqa: E402


def directory(buf):
    n_sections = struct.unpack_from("<I", buf, 12)[0]
    out = {}
    for i in range(n_sections):
        pos = vrtt.HEADER.size + i * vrtt.ENTRY.size
        name, elem, _, offset, count = vrtt.ENTRY.unpack_from(buf, pos)
        out[name.rstrip(b"\0").decode()] = (pos, elem, offset, count)
    return out


def rechecksum(buf):
    data_offset = struct.unpack_from("<Q", buf, 48)[0]
    struct.pack_into("<Q", buf, 56, vrtt.fnv1a64(memoryview(buf)[data_offset:]))


def rename(buf, old, new):
    pos = directory(buf)[old][0]
    buf[pos:pos + 16] = new.encode().ljust(16, b"\0")


def swap_interval(buf):
    frm, to = struct.unpack_from("<qq", buf, 16)
    struct.pack_into("<qq", buf, 16, to, frm)


def bump_last(buf, name):
    _, elem, offset, count = directory(buf)[name]
    fmt = {4: "<I", 8: "<Q"}[elem]
    pos = offset + (count - 1) * elem
    struct.pack_into(fmt, buf, pos, struct.unpack_from(fmt, buf, pos)[0] + 1)


def unsort(buf, name):
    """begin[1] := begin[2] + 1: not monotone, while begin[0] = 0 and the final
    entry (the payload length) stay intact, so only the monotonicity check fires."""
    _, elem, offset, count = directory(buf)[name]
    fmt = {4: "<I", 8: "<Q"}[elem]
    if count < 3:
        raise RuntimeError(f"{name}: too short to unsort")
    nxt = struct.unpack_from(fmt, buf, offset + 2 * elem)[0]
    struct.pack_into(fmt, buf, offset + elem, nxt + 1)


def drop_section(src, dst, name):
    """Re-lay the file out without one section (consistent directory, no unknown
    name), so only the missing-section check fires."""
    meta, s = vrtt.load(src)
    vrtt.write(dst, meta, s, order=[x for x in vrtt.ORDER if x != name])


def move_window(buf):
    _, _, offset, count = directory(buf)["trip_last_arr"]
    start = max(last for (last,) in struct.iter_unpack("<q", buf[offset:offset + 8 * count])) + 20
    end = max(start, struct.unpack_from("<q", buf, 24)[0])
    struct.pack_into("<qq", buf, 16, start, end)


def corrupt_location_id(buf):
    _, _, offset, _ = directory(buf)["loc_id_bytes"]
    buf[offset] = 0xff


def flip_payload(buf):
    _, elem, offset, count = directory(buf)["ev_arr"]
    buf[offset + (count // 2) * elem] ^= 0x01


# (label, corruption, recompute checksum, expected rejection reason)
CASES = [
    ("duplicate section (loc_lon renamed to loc_lat)", lambda b: rename(b, "loc_lon", "loc_lat"), True,
     "duplicate section 'loc_lat'"),
    ("unknown section (loc_src renamed to loc_srcx)", lambda b: rename(b, "loc_src", "loc_srcx"), True,
     "unknown section 'loc_srcx'"),
    ("missing section (loc_src dropped, file re-laid out)", "drop:loc_src", False,
     "missing sections ['loc_src']"),
    ("reversed export interval", swap_interval, True, "export interval: from > to"),
    ("mismatched index array (fp_begin end + 1)", lambda b: bump_last(b, "fp_begin"), True,
     "fp_begin: ends at"),
    ("non-monotone index array (ev_begin[1], final entry intact)", lambda b: unsort(b, "ev_begin"), True,
     "ev_begin: not monotone"),
    ("outside-window", move_window, False, "trip 0: outside export interval"),
    ("invalid-utf8-id", corrupt_location_id, True, "location 0: invalid UTF-8 ID"),
    ("flipped payload byte (ev_arr), checksum kept", flip_payload, False, "checksum mismatch"),
]


def check(path):
    r = subprocess.run([sys.executable, os.path.join(HERE, "vrtt.py"), "check", path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    return r.returncode, r.stderr.strip()


def synthetic(path):
    """Three locations, one footpath, one route A -> C, two trips."""
    ids = [b"A", b"B", b"C"]
    offsets = [0]
    for i in ids:
        offsets.append(offsets[-1] + len(i))
    s = {
        "loc_parent": [-1, -1, -1], "loc_type": [1, 1, 1], "loc_transfer": [2, 2, 2], "loc_base": [0, 1, 2],
        "loc_traffic": [1, 0, 1], "loc_src": [0, 0, 0], "loc_lat": [50.0, 50.001, 50.1], "loc_lon": [8.0, 8.0, 8.1],
        "loc_id_offsets": offsets, "loc_id_bytes": list(b"".join(ids)),
        "fp_begin": [0, 1, 1, 1], "fp_to": [1], "fp_minutes": [5],
        "rs_begin": [0, 2], "rs_loc": [0, 2], "rs_flags": [3, 3],
        "trip_route": [0, 0], "trip_transport": [0, 1], "trip_day": [0, 0],
        "trip_first_dep": [100, 120], "trip_last_arr": [110, 130],
        "ev_begin": [0, 2, 4], "ev_arr": [-1, 110, -1, 130], "ev_dep": [100, -1, 120, -1],
    }
    vrtt.write(path, dict(from_unix_minutes=90, to_unix_minutes=200, days_from=0, days_to=1), s)


def main():
    if sys.argv[1] == "--synthetic":
        tmp = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="vrtt-test-")
    else:
        src = sys.argv[1]
        tmp = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="vrtt-test-")
    Path(tmp).mkdir(parents=True, exist_ok=True)
    if sys.argv[1] == "--synthetic":
        src = os.path.join(tmp, "synthetic.vrtt")
        synthetic(src)
    ok = True
    rc, err = check(src)
    print(f"valid {src}: rc={rc} {err}")
    ok &= rc == 0
    for i, (label, corrupt, rechk, reason) in enumerate(CASES):
        path = os.path.join(tmp, f"case{i}.vrtt")
        if isinstance(corrupt, str) and corrupt.startswith("drop:"):
            drop_section(src, path, corrupt[5:])
        else:
            buf = bytearray(open(src, "rb").read())
            corrupt(buf)
            if rechk:
                rechecksum(buf)
            open(path, "wb").write(buf)
        rc, err = check(path)
        exact = label in ("outside-window", "invalid-utf8-id")
        good = rc == 2 and (err == f"invalid: {reason}" if exact else reason in err)
        ok &= good
        print(f"{'REJECTED' if good else 'WRONG'} {label}: rc={rc} {err} (expected reason: {reason})")
        os.remove(path)
    print("vrtt_test: OK" if ok else "vrtt_test: FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
