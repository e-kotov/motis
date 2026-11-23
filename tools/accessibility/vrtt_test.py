#!/usr/bin/env python3
"""Negative tests for `vrtt.py check`.

    vrtt_test.py VALID.vrtt [TMP_DIR]
    vrtt_test.py --synthetic [TMP_DIR]   (writes a small valid export with vrtt.write first)

The valid file must pass. Each corrupted copy must be rejected (exit 2):
duplicate section (replacing a required one), unknown / missing section,
reversed export interval, mismatched CSR index array, non-monotone index array
and a flipped payload byte. Except for the last case the checksum is recomputed,
so only the structural checks can catch them. Exit 0 if all expectations hold.
"""
import os
import struct
import subprocess
import sys
import tempfile

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
    _, elem, offset, count = directory(buf)[name]
    fmt = {4: "<I", 8: "<Q"}[elem]
    # find i with begin[i] < begin[i+1] and swap them
    for i in range(1, count - 1):
        a = struct.unpack_from(fmt, buf, offset + i * elem)[0]
        b = struct.unpack_from(fmt, buf, offset + (i + 1) * elem)[0]
        if a < b:
            struct.pack_into(fmt, buf, offset + i * elem, b)
            struct.pack_into(fmt, buf, offset + (i + 1) * elem, a)
            return
    raise RuntimeError(f"{name}: nothing to unsort")


def flip_payload(buf):
    _, elem, offset, count = directory(buf)["ev_arr"]
    buf[offset + (count // 2) * elem] ^= 0x01


CASES = [
    ("duplicate section (loc_lon renamed to loc_lat)", lambda b: rename(b, "loc_lon", "loc_lat"), True),
    ("unknown section / missing loc_src", lambda b: rename(b, "loc_src", "loc_srcx"), True),
    ("reversed export interval", swap_interval, True),
    ("mismatched index array (fp_begin end + 1)", lambda b: bump_last(b, "fp_begin"), True),
    ("non-monotone index array (ev_begin)", lambda b: unsort(b, "ev_begin"), True),
    ("flipped payload byte (ev_arr), checksum kept", flip_payload, False),
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
        src = os.path.join(tmp, "synthetic.vrtt")
        synthetic(src)
    else:
        src = sys.argv[1]
        tmp = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="vrtt-test-")
    ok = True
    rc, err = check(src)
    print(f"valid {src}: rc={rc} {err}")
    ok &= rc == 0
    for i, (label, corrupt, rechk) in enumerate(CASES):
        buf = bytearray(open(src, "rb").read())
        corrupt(buf)
        if rechk:
            rechecksum(buf)
        path = os.path.join(tmp, f"case{i}.vrtt")
        open(path, "wb").write(buf)
        rc, err = check(path)
        good = rc == 2
        ok &= good
        print(f"{'REJECTED' if good else 'ACCEPTED (WRONG)'} {label}: rc={rc} {err}")
        os.remove(path)
    print("vrtt_test: OK" if ok else "vrtt_test: FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
