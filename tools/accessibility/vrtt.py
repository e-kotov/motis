#!/usr/bin/env python3
"""Reference reader for `motis-accessibility export-tt --format bin` (timetable.vrtt).

    vrtt.py check FILE            verify magic, version, directory, layout, checksum
    vrtt.py to-csv FILE OUT_DIR   regenerate the CSV export's five tables byte for byte

Standard library only. The layout is described in tools/accessibility/README.md.
"""
import array
import os
import struct
import sys

HEADER = struct.Struct("<8sIIqqqqQQ")  # 64 bytes
ENTRY = struct.Struct("<16sIIQQ")  # 40 bytes
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
TYPES = {  # section -> array typecode (element size must match the directory)
    "loc_parent": "i", "loc_type": "B", "loc_transfer": "H", "loc_base": "I",
    "loc_traffic": "B", "loc_src": "I", "loc_lat": "d", "loc_lon": "d",
    "loc_id_offsets": "Q", "loc_id_bytes": "B", "fp_begin": "I", "fp_to": "I",
    "fp_minutes": "H", "rs_begin": "I", "rs_loc": "I", "rs_flags": "B",
    "trip_route": "I", "trip_transport": "I", "trip_day": "I", "trip_first_dep": "q",
    "trip_last_arr": "q", "ev_begin": "Q", "ev_arr": "q", "ev_dep": "q",
}


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def load(path, verify_checksum=True):
    buf = open(path, "rb").read()
    magic, version, n_sections, frm, to, dfrom, dto, data_offset, checksum = HEADER.unpack_from(buf, 0)
    if magic != b"VRTTEXP1" or version != 1:
        raise ValueError(f"bad magic/version {magic!r} {version}")
    if data_offset != HEADER.size + n_sections * ENTRY.size:
        raise ValueError("bad data_offset")
    sections = {}
    prev_end = data_offset
    for i in range(n_sections):
        name, elem, reserved, offset, count = ENTRY.unpack_from(buf, HEADER.size + i * ENTRY.size)
        name = name.rstrip(b"\0").decode()
        code = TYPES[name]
        if array.array(code).itemsize != elem or reserved != 0 or offset % 8 or offset < prev_end:
            raise ValueError(f"bad directory entry {name}")
        if any(buf[prev_end:offset]):
            raise ValueError(f"non-zero padding before {name}")
        a = array.array(code)
        a.frombytes(buf[offset:offset + elem * count])
        if sys.byteorder != "little":
            a.byteswap()
        sections[name] = a
        prev_end = offset + elem * count
    if prev_end + (-prev_end % 8) != len(buf) or any(buf[prev_end:]):
        raise ValueError("bad file end")
    if verify_checksum and fnv1a64(memoryview(buf)[data_offset:]) != checksum:
        raise ValueError("checksum mismatch")
    meta = dict(from_unix_minutes=frm, to_unix_minutes=to, days_from=dfrom, days_to=dto,
                checksum=f"{checksum:016x}")
    return meta, sections


def to_csv(s, out):
    os.makedirs(out, exist_ok=True)
    n_loc = len(s["loc_parent"])
    ids = bytes(s["loc_id_bytes"])
    off = s["loc_id_offsets"]
    with open(os.path.join(out, "locations.csv"), "w", newline="\n") as f:
        f.write("loc,id,src,lat,lon,parent,type,transfer_time,base,traffic\n")
        for i in range(n_loc):
            f.write("%d,%s,%d,%.7f,%.7f,%d,%d,%d,%d,%d\n" % (
                i, ids[off[i]:off[i + 1]].decode(), s["loc_src"][i], s["loc_lat"][i], s["loc_lon"][i],
                s["loc_parent"][i], s["loc_type"][i], s["loc_transfer"][i], s["loc_base"][i],
                s["loc_traffic"][i]))
    with open(os.path.join(out, "footpaths.csv"), "w", newline="\n") as f:
        f.write("from,to,minutes\n")
        b = s["fp_begin"]
        for i in range(n_loc):
            for j in range(b[i], b[i + 1]):
                f.write("%d,%d,%d\n" % (i, s["fp_to"][j], s["fp_minutes"][j]))
    with open(os.path.join(out, "route_stops.csv"), "w", newline="\n") as f:
        f.write("route,seq,loc,in_allowed,out_allowed\n")
        b = s["rs_begin"]
        for r in range(len(b) - 1):
            for k, j in enumerate(range(b[r], b[r + 1])):
                fl = s["rs_flags"][j]
                f.write("%d,%d,%d,%d,%d\n" % (r, k, s["rs_loc"][j], fl & 1, (fl >> 1) & 1))
    with open(os.path.join(out, "trips.csv"), "w", newline="\n") as f, \
            open(os.path.join(out, "events.csv"), "w", newline="\n") as e:
        f.write("trip,route,transport,day,first_dep,last_arr\n")
        e.write("trip,seq,arr,dep\n")
        b = s["ev_begin"]
        for t in range(len(s["trip_route"])):
            f.write("%d,%d,%d,%d,%d,%d\n" % (t, s["trip_route"][t], s["trip_transport"][t], s["trip_day"][t],
                                            s["trip_first_dep"][t], s["trip_last_arr"][t]))
            for k, j in enumerate(range(b[t], b[t + 1])):
                e.write("%d,%d,%d,%d\n" % (t, k, s["ev_arr"][j], s["ev_dep"][j]))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("check", "to-csv"):
        print(__doc__)
        sys.exit(1)
    meta, s = load(sys.argv[2])
    counts = {k: len(v) for k, v in s.items()}
    print({**meta, "n_locations": counts["loc_parent"], "n_footpaths": counts["fp_to"],
           "n_route_stops": counts["rs_loc"], "n_trips": counts["trip_route"], "n_events": counts["ev_arr"]})
    if sys.argv[1] == "to-csv":
        to_csv(s, sys.argv[3])


if __name__ == "__main__":
    main()
