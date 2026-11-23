# Offline many-origin accessibility batch

Two tools:

- `motis-accessibility` (C++): a CMake target in this repository, linked against
  `motislib`. It uses the stock timetable data directory written by `motis import`.
- `vr-access` (Rust, `tools/accessibility/rust`): writes the inputs, reduces and
  compares the outputs, and runs an independent oracle.

The transit search is the unmodified Nigiri `one_to_all` RAPTOR. It runs once per
origin and departure minute, so it answers fixed-departure queries, not range or
profile queries. Access and egress connectors are computed once per point and
reused across all departures and origins.

## `motis-accessibility`

```
motis-accessibility stops      --data DIR --out stops.csv [--footpaths footpaths.csv]
motis-accessibility connectors --data DIR --points cells.csv --dir forward|backward \
                               --max-seconds S [--pedestrian-speed M/S] [--max-matching-distance M] [--threads T] --out C.csv
motis-accessibility direct     --data DIR --origins cells.csv --dests cells.csv --max-seconds S --radius-m R \
                               [--pedestrian-speed M/S] [--threads T] --out D.csv
motis-accessibility route      --data DIR --n-origins N [--n-dests M] --access A.csv [--egress E.csv] [--direct D.csv] \
                               --dep-start-unix-minutes T0 [--dep-count C] [--dep-step-minutes S] --max-travel-minutes CAP \
                               [--max-transfers K] [--target dests|stops] [--origin-from I --origin-to J | --origin-list I,J,..] \
                               [--with-transfers] [--with-egress-stop] [--threads T] --out stream.bin [--timing-json t.json]
```

- `stops` writes `stop,id,src,lat,lon,parent,type,transfer_time,traffic` for every
  Nigiri location. `traffic` = any route serves the location. With `--footpaths` it
  also writes the profile-0 footpath graph as `from,to,minutes`.
- `connectors` runs stock `motis::ep::routing::get_offsets` (mode WALK, profile
  FOOT, OSM). It writes `point,stop,minutes`; `ceil(seconds / 60)` is the stock
  convention. `forward` gives access; `backward` gives egress, the direction used by
  the one-to-many endpoint for its destinations.
- `direct` runs stock `one_to_many_direct` (WALK) to every destination within
  `--radius-m` straight-line distance and writes `point,stop,minutes`. Here `stop`
  holds the destination index and minutes are `ceil(seconds / 60)`.
- `route` reduces each origin and departure as follows:
  - Transit candidate for destination *d*: the fastest arrival at stop *s* in
    rounds *k* ≥ 1, plus egress(*s*, *d*). The stop arrival is the vehicle arrival or
    a footpath arrival, with no final transfer buffer. Transfers = *k* − 1.
  - Direct candidate: the direct walk. It wins ties.
  - Final value: the lexicographic minimum of (minutes, transfers). Totals above
    CAP are unreachable; CAP itself is inclusive.
  - `--target stops` instead writes the fastest arrival at every location, including
    round 0, as in the stock `/api/v6/one-to-all` endpoint.

  This is the same rule as stock `/api/experimental/one-to-many-intermodal`, except
  that stock does not apply the cap to transit totals.

## Stream format (`VRTTMATX`, version 1, little-endian)

The header is 56 bytes:

- `char[8]` magic
- `u32` version
- `u32` n_origins_total, `u32` n_dests, `u32` n_departures
- `u32` origin_from, `u32` origin_to
- `u16` max_travel_minutes
- `u8` flags (bit 0: transfers block; bit 1: egress-stop block)
- `u8` target (0 = destinations, 1 = stops)
- `u32[5]` reserved

It is followed by `n_departures × i64` departure times (UTC unix minutes). Records
come in any order. Each record is:

- `u32` origin
- `u8` tt[dep][dest]: 0..CAP minutes, 255 = unreachable
- optional `u8` transfers[dep][dest]: 0.., 254 = direct walk, 255 = unreachable
- optional `u32` egress_stop[dep][dest]: 0xFFFFFFFF = none

The payload is arrival times only. **No journeys are reconstructed.**

## `vr-access`

- `departures`: converts a local window to UTC minutes.
- `straight-connectors`, `straight-direct`: haversine distance (R = 6,371 km),
  `ceil` at the given speed, inclusive radius. They use only stops with traffic.
- `reduce`: population-weighted cumulative accessibility per origin for the
  semantics `fixed`, `best`, `meancount` and `meantt`; see `src/reduce.rs`. Options:
  `--exclude-self`, `--continuous-points`.
- `compare`: exact typed comparison of two streams.
- `motis-queries`, `from-motis-batch`: control runs against the stock `motis batch`
  endpoint.
- `oracle`: an independent GTFS connection scan that shares only the stop index,
  transfer times and footpaths with Nigiri. Its rules are listed in `src/oracle.rs`.
- `inspect`: prints a stream's header and per-departure reachable-cell counts.
