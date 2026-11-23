#!/usr/bin/env python3
"""Reference reader for `motis-accessibility export-tt --format bin` (timetable.vrtt).

    vrtt.py check FILE            verify magic, version, directory (each required section\n                                  exactly once), layout, checksum, interval order and the\n                                  consistency of every array (lengths, CSR indexes, bounds);\n                                  exit 2 if invalid
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


class Invalid(ValueError):
    pass


def require(cond, msg):
    if not cond:
        raise Invalid(msg)


def check_csr(begin, n_rows, payload_len, name):
    require(len(begin) == n_rows + 1, f"{name}: {len(begin)} entries, expected {n_rows + 1}")
    require(begin[0] == 0, f"{name}: does not start at 0")
    require(begin[-1] == payload_len, f"{name}: ends at {begin[-1]}, payload has {payload_len}")
    require(all(begin[i] <= begin[i + 1] for i in range(n_rows)), f"{name}: not monotone")


def validate(meta, s):
    """Structural checks of a loaded export (raises Invalid)."""
    require(meta["from_unix_minutes"] <= meta["to_unix_minutes"], "export interval: from > to")
    require(meta["days_from"] <= meta["days_to"], "internal interval: days_from > days_to")
    n = len(s["loc_parent"])
    for k in ("loc_type", "loc_transfer", "loc_base", "loc_traffic", "loc_src", "loc_lat", "loc_lon"):
        require(len(s[k]) == n, f"{k}: {len(s[k])} entries, expected {n}")
    check_csr(s["loc_id_offsets"], n, len(s["loc_id_bytes"]), "loc_id_offsets")
    require(all(-1 <= x < n for x in s["loc_parent"]), "loc_parent out of range")
    require(all(x < n for x in s["loc_base"]), "loc_base out of range")
    require(all(x <= 3 for x in s["loc_type"]), "loc_type out of range")
    require(all(x <= 1 for x in s["loc_traffic"]), "loc_traffic not 0/1")
    require(len(s["fp_to"]) == len(s["fp_minutes"]), "fp_to / fp_minutes lengths differ")
    check_csr(s["fp_begin"], n, len(s["fp_to"]), "fp_begin")
    require(all(x < n for x in s["fp_to"]), "fp_to out of range")
    n_routes = len(s["rs_begin"]) - 1
    require(n_routes >= 0, "rs_begin empty")
    require(len(s["rs_loc"]) == len(s["rs_flags"]), "rs_loc / rs_flags lengths differ")
    check_csr(s["rs_begin"], n_routes, len(s["rs_loc"]), "rs_begin")
    require(all(x < n for x in s["rs_loc"]), "rs_loc out of range")
    require(all(x < 4 for x in s["rs_flags"]), "rs_flags out of range")
    n_trips = len(s["trip_route"])
    for k in ("trip_transport", "trip_day", "trip_first_dep", "trip_last_arr"):
        require(len(s[k]) == n_trips, f"{k}: {len(s[k])} entries, expected {n_trips}")
    require(all(r < n_routes for r in s["trip_route"]), "trip_route out of range")
    require(len(s["ev_arr"]) == len(s["ev_dep"]), "ev_arr / ev_dep lengths differ")
    check_csr(s["ev_begin"], n_trips, len(s["ev_arr"]), "ev_begin")
    rb, eb = s["rs_begin"], s["ev_begin"]
    for t in range(n_trips):
        r = s["trip_route"][t]
        require(eb[t + 1] - eb[t] == rb[r + 1] - rb[r], f"trip {t}: event count differs from its route length")
        require(eb[t + 1] > eb[t], f"trip {t}: no events")
        require(s["ev_arr"][eb[t]] == -1 and s["ev_dep"][eb[t + 1] - 1] == -1, f"trip {t}: missing -1 end markers")
        require(s["trip_first_dep"][t] <= s["trip_last_arr"][t], f"trip {t}: first departure after last arrival")


def load(path, verify_checksum=True):
    buf = open(path, "rb").read()
    require(len(buf) >= HEADER.size, "file shorter than the header")
    magic, version, n_sections, frm, to, dfrom, dto, data_offset, checksum = HEADER.unpack_from(buf, 0)
    require(magic == b"VRTTEXP1", f"bad magic {magic!r}")
    require(version == 1, f"unsupported version {version}")
    require(data_offset == HEADER.size + n_sections * ENTRY.size, "bad data_offset")
    require(len(buf) >= data_offset, "file shorter than the section directory")
    sections = {}
    prev_end = data_offset
    for i in range(n_sections):
        name, elem, reserved, offset, count = ENTRY.unpack_from(buf, HEADER.size + i * ENTRY.size)
        name = name.rstrip(b"\0").decode(errors="replace")
        require(name in TYPES, f"unknown section {name!r}")
        require(name not in sections, f"duplicate section {name!r}")
        code = TYPES[name]
        require(array.array(code).itemsize == elem, f"{name}: element size {elem}")
        require(reserved == 0, f"{name}: reserved field not 0")
        require(offset % 8 == 0 and offset >= prev_end, f"{name}: offset {offset} not aligned / overlapping")
        require(offset + elem * count <= len(buf), f"{name}: extends past the end of the file")
        require(not any(buf[prev_end:offset]), f"non-zero padding before {name}")
        a = array.array(code)
        a.frombytes(buf[offset:offset + elem * count])
        if sys.byteorder != "little":
            a.byteswap()
        sections[name] = a
        prev_end = offset + elem * count
    missing = sorted(set(TYPES) - set(sections))
    require(not missing, f"missing sections {missing}")
    require(prev_end + (-prev_end % 8) == len(buf) and not any(buf[prev_end:]), "bad file end")
    if verify_checksum:
        require(fnv1a64(memoryview(buf)[data_offset:]) == checksum, "checksum mismatch")
    meta = dict(from_unix_minutes=frm, to_unix_minutes=to, days_from=dfrom, days_to=dto,
                checksum=f"{checksum:016x}")
    validate(meta, sections)
    return meta, sections


ORDER = ["loc_parent", "loc_type", "loc_transfer", "loc_base", "loc_traffic", "loc_src", "loc_lat",
         "loc_lon", "loc_id_offsets", "loc_id_bytes", "fp_begin", "fp_to", "fp_minutes", "rs_begin",
         "rs_loc", "rs_flags", "trip_route", "trip_transport", "trip_day", "trip_first_dep",
         "trip_last_arr", "ev_begin", "ev_arr", "ev_dep"]


def write(path, meta, sections):
    """Write sections (name -> sequence) as timetable.vrtt, in the exporter's layout."""
    data_offset = HEADER.size + len(ORDER) * ENTRY.size
    blobs, entries, offset = [], [], data_offset
    for name in ORDER:
        a = array.array(TYPES[name], sections[name])
        if sys.byteorder != "little":
            a.byteswap()
        b = a.tobytes()
        offset += -offset % 8
        entries.append(ENTRY.pack(name.encode().ljust(16, b"\0"), a.itemsize, 0, offset, len(a)))
        blobs.append((offset, b))
        offset += len(b)
    end = offset + (-offset % 8)
    data = bytearray(end - data_offset)
    for off, b in blobs:
        data[off - data_offset:off - data_offset + len(b)] = b
    header = HEADER.pack(b"VRTTEXP1", 1, len(ORDER), meta["from_unix_minutes"], meta["to_unix_minutes"],
                         meta["days_from"], meta["days_to"], data_offset, fnv1a64(data))
    with open(path, "wb") as f:
        f.write(header + b"".join(entries) + bytes(data))


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
    try:
        meta, s = load(sys.argv[2])
    except Invalid as e:
        print(f"invalid: {e}", file=sys.stderr)
        sys.exit(2)
    counts = {k: len(v) for k, v in s.items()}
    print({**meta, "n_locations": counts["loc_parent"], "n_footpaths": counts["fp_to"],
           "n_route_stops": counts["rs_loc"], "n_trips": counts["trip_route"], "n_events": counts["ev_arr"]})
    if sys.argv[1] == "to-csv":
        to_csv(s, sys.argv[3])


if __name__ == "__main__":
    main()
