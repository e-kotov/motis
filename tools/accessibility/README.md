# Offline many-origin accessibility batch

Two tools:

- `motis-accessibility` (C++): a CMake target in this repository, linked against
  `motislib`. It reads the stock timetable data directory written by `motis import`.
- `vr-access` (Rust, `tools/accessibility/rust`): writes the inputs, reduces and
  compares the outputs, gates their validity, and runs an independent oracle.

The transit search is the unmodified Nigiri `one_to_all` RAPTOR. It runs once per
origin and departure minute, so it answers fixed-departure queries, not range or
profile queries. Access and egress connectors are computed once per point and
reused across all departures and origins.

## `motis-accessibility`

```
motis-accessibility stops      --data DIR --out stops.csv [--footpaths footpaths.csv]
motis-accessibility connectors --data DIR (--points cells.csv | --stop-points stops.csv) --dir forward|backward \
                               --max-seconds S [--pedestrian-speed M/S] [--max-matching-distance M] [--threads T] --out C.csv
motis-accessibility direct     --data DIR --origins cells.csv --dests cells.csv --max-seconds S --radius-m R \
                               [--pedestrian-speed M/S] [--threads T] --out D.csv
motis-accessibility stream-decode --in sparse.bin --out dense.bin
motis-accessibility export-tt  --data DIR --from-unix-minutes A --to-unix-minutes B [--format csv|bin] --out DIR
motis-accessibility route      --data DIR --n-origins N [--n-dests M] --access A.csv [--egress E.csv] [--direct D.csv] \
                               --dep-start-unix-minutes T0 [--dep-count C] [--dep-step-minutes S] --max-travel-minutes CAP \
                               [--max-transfers 0..14] [--target dests|stops] [--egress-rounds transit|all] \
                               [--origin-from I --origin-to J | --origin-list I,J,..] [--with-transfers] [--with-egress-stop] \
                               [--threads T] [--pin-threads] [--reuse-state] [--range] [--discard] [--out stream.bin] [--timing-json t.json] \
                               [--egress-stats] [--row-digest] [--stream-encoding dense|sparse] \
                               [--r5r-out DIR [--percentiles P,..] [--opportunities cells.csv --opportunity-columns C,.. \
                                --cutoffs MIN,.. [--decay step|linear:W|exponential|fixed-exponential:K|logistic:SD]]]
```

### `stops`

Writes `stop,id,src,lat,lon,parent,type,transfer_time,traffic` for every Nigiri
location. Columns:

- `type`: 0 generated track, 1 track, 2 station, 3 virtual (empty `id`).
- `traffic`: any route serves the location.

`--footpaths` also writes the profile-0 footpath graph as `from,to,minutes`.

### `connectors`

Runs stock `motis::ep::routing::get_offsets` (WALK, FOOT, OSM) and writes
`point,stop,minutes`, using `ceil(seconds / 60)`.

- `--points` takes coordinates.
- `--stop-points` takes Nigiri location indices and starts the way the stock
  endpoints start from a stop: walking offsets plus zero-cost equivalent and meta
  stops.
- `--dir forward` gives access; `--dir backward` gives egress, the direction used by
  the one-to-many endpoint.

### `direct`

Runs stock `one_to_many_direct` (WALK) to every destination within `--radius-m`
straight-line distance. Output: `point,stop,minutes`, where `stop` holds the
destination index.

### `export-tt`

Writes what the pinned Nigiri RAPTOR scans, for an external kernel: profile 0 and
the scheduled timetable only. It covers locations (base and parent mapping, transfer
times), the directed footpath graph, route stop sequences with boarding and alighting
flags, and every transport instance (transport, day) whose first departure and last
arrival overlap [A, B], with absolute UTC event times in minutes.

- `--format csv` (default) writes `locations.csv`, `footpaths.csv`,
  `route_stops.csv`, `trips.csv`, `events.csv` and `export.json`.
- `--format bin` writes the same tables as one file, `timetable.vrtt`, plus
  `export.json` (with the checksum), for zero-copy loading. `vrtt.py` is a
  standard-library reference reader:
  - `vrtt.py check FILE` exits 2 on any invalid file. It checks the magic, the
    version, each required section exactly once, element sizes, alignment and
    padding, the checksum, `from ≤ to`, and every array's consistency: lengths,
    CSR indexes starting at 0, monotone and ending at their payload, references
    in bounds, and per-trip event counts equal to the route length.
  - `vrtt.py to-csv FILE DIR` regenerates the CSV export byte for byte.
  - `vrtt.write()` writes the same layout (a rewrite of an export is byte-identical).
  - `vrtt_test.py FILE` (or `--synthetic`, a small file written with `vrtt.write`) checks that `check` rejects corrupted copies of a valid
    file: a duplicate, unknown or missing section, a reversed interval, a
    mismatched or non-monotone index array, or a flipped payload byte.

`timetable.vrtt` layout (little-endian):

| Part | Content |
|---|---|
| Header, 64 bytes | `char[8]` magic `VRTTEXP1`; `u32` version = 1; `u32` n_sections; `i64` from, to (unix minutes); `i64` internal interval days from, to (days since epoch); `u64` data_offset; `u64` checksum = FNV-1a 64 over bytes [data_offset, end of file) |
| Directory, n_sections × 40 bytes | `char[16]` name (NUL padded); `u32` element size; `u32` reserved = 0; `u64` absolute offset (8-byte aligned); `u64` element count |
| Data | the sections in directory order; zero padding between them to 8-byte alignment |

Sections (n = locations, R = routes, T = trips). CSR arrays have one entry more
than their rows; rows of the next array run from `begin[i]` to `begin[i+1]`.

| Section | Type | Count | Meaning |
|---|---|---|---|
| `loc_parent` | i32 | n | parent location, −1 if none |
| `loc_type` | u8 | n | Nigiri location type (0 generated track, 1 track, 2 station, 3 virtual) |
| `loc_transfer` | u16 | n | transfer time, minutes |
| `loc_base` | u32 | n | base location (arrivals are recorded there) |
| `loc_traffic` | u8 | n | 1 if any route serves the location |
| `loc_src` | u32 | n | source (dataset) index |
| `loc_lat`, `loc_lon` | f64 | n | coordinates |
| `loc_id_offsets` | u64 | n + 1 | CSR into `loc_id_bytes` (UTF-8 ids) |
| `loc_id_bytes` | u8 | – | concatenated ids |
| `fp_begin` | u32 | n + 1 | CSR by source location into `fp_to` / `fp_minutes` |
| `fp_to`, `fp_minutes` | u32, u16 | footpaths | target location, duration in minutes |
| `rs_begin` | u32 | R + 1 | CSR by route into `rs_loc` / `rs_flags` |
| `rs_loc`, `rs_flags` | u32, u8 | route stops | location; bit 0 boarding allowed, bit 1 alighting allowed |
| `trip_route`, `trip_transport`, `trip_day` | u32 | T | route, Nigiri transport index, day index of the instance |
| `trip_first_dep`, `trip_last_arr` | i64 | T | unix minutes |
| `ev_begin` | u64 | T + 1 | CSR by trip into `ev_arr` / `ev_dep`, in route stop order |
| `ev_arr`, `ev_dep` | i64 | events | unix minutes; −1 for no arrival at the first stop and no departure at the last |

### `route`

Per origin and departure:

- **Destinations** (`--target dests`):
  - Transit candidate for destination *d*: the fastest arrival at stop *s* in rounds
    k ≥ 1, plus egress(*s*, *d*). The stop arrival is the vehicle arrival or a
    footpath arrival, with no final transfer buffer. Transfers = k − 1.
  - `--egress-rounds all` also admits round k = 0, i.e. walking origin → stop →
    destination without a vehicle (transfer code 253).
  - Direct candidate: the direct walk (code 254). It wins ties.
  - Final value: the lexicographic minimum of (minutes, transfers). Totals above CAP
    are unreachable; CAP itself is inclusive.
  - This is the stock `/api/experimental/one-to-many-intermodal` rule, except that
    stock does not cap transit totals.
  - Each stop's egress connectors are sorted by (minutes, destination) when they
    are loaded. The egress loop stops at a stop's first connector whose total
    exceeds CAP, so connectors that cannot count are never visited. Within one stop
    every connector shares the stop's arrival and transfer count, so the order does
    not change any value, transfer code or egress stop.
- **Stops** (`--target stops`): the fastest arrival at every location, including
  round 0, as in stock `/api/v6/one-to-all`. Here the transfers block holds **k**,
  the number of vehicles used (0 = start or walk only), not k − 1.

Bounds and modes:

- `--max-transfers` must be in **[0, 14]**, Nigiri's `kMaxTransfers`. Values outside
  the range are rejected before narrowing.
- `--max-travel-minutes` must be in **[0, 1440]**.
- `--discard` computes everything and writes no records. It is for timing only.
- `--egress-stats` adds an `egress_stats` object to the timing JSON. It counts,
  summed over queries: stops with egress connectors, those reached (a valid
  arrival in an admitted round), their egress links, the links within the cap and
  the links that improved a destination. Per finished row (one departure of one
  origin) it counts reached cells and sizes the row dense, as a presence bitmap
  plus reached cells, and as the smaller of the two; `reached_decile[i]` counts
  rows whose reached fraction lies in [i/10, (i+1)/10). Outputs are unchanged;
  without the flag the egress loop carries no counters.
- `--row-digest` adds `row_digest` to the timing JSON: per record, wyhash
  (`cista::hash`) chained over the bytes the stream record holds (origin, travel
  times, optional transfers and egress-stop blocks), combined over records by a
  wrapping 64-bit sum and an xor. It does not depend on thread order and also
  works with `--discard`, so two builds can be checked for identical rows without
  writing the stream.
- `row_init_thread_ms` in the timing JSON is the time spent resetting the row
  buffers, summed over threads.
- `--reuse-state` keeps one Nigiri `raptor_state` per worker thread instead of a
  fresh one per query; every query still starts from a reset state, so outputs are
  unchanged. With a Nigiri that provides `one_to_all(tt, rtt, q, raptor_state&)` the
  stock function runs into the reused state; otherwise the tool runs its body.
- `--range` runs a multi-departure window as Range-RAPTOR: per origin one raptor
  handles the departures latest first, as Nigiri's own range search does. Only
  `next_start_time()` resets between departures; round times and round arrivals
  carry over from later departures, which stay valid because one can wait. Each
  departure is then finished on a copy, with the strict-improvement rule and its
  own cap, and read exactly as in the default mode. The output is meant to be
  identical to one `one_to_all` per departure; check it with `vr-access compare`
  on your data. The window must lie within one Nigiri day base: the departure rounded to the
  nearest UTC day (`std::chrono::round`, which sends a tie at exactly 12:00 UTC to
  the even day number). Otherwise the tool exits 2 and names the first departure
  whose day differs, where the next window should start. For example, a window
  starting 2026-11-25 12:00 UTC is refused at 12:01, while windows starting at
  12:01 that day, or at 2026-11-26 12:00, are accepted.
- `--pin-threads` pins worker *i* to the *i*-th CPU the process may use (its
  cpuset). The timing JSON records the allowed CPUs.
- The timing JSON records the effective `max_transfers`, `max_travel_minutes`,
  `egress_rounds`, `discard` and the stage times.

Nigiri `one_to_all` keeps a round's arrival at a stop only if it is strictly earlier
than all earlier rounds. With stock egress (k ≥ 1 only), a stop reachable on foot
from the origin no later than by transit therefore offers no transit egress.

### r5r-style outputs (`route --r5r-out DIR`)

These outputs are computed per origin while routing, over the departures of the
window, so no raw per-departure matrix is written. Without `--out`, no stream is
written at all. The definitions are ports of Conveyal R5 (`analyst/TravelTimeReducer.java`
and `analyst/decay/*.java`, `conveyal/r5` dev `500ce263`), which `r5r` wraps.

- **Travel-time percentiles** (like `r5r::travel_time_matrix()`). For each
  destination, its travel times over the departures are sorted, with unreached
  last. Percentile *p* is the element at index ⌈p/100 · n⌉ − 1: no interpolation, the
  smallest value with no more than *p* % of the times below it.
  - Percentiles are **integers 1–100**, as R5's `int[]`, and the rank is computed
    exactly as (p·n + 99) / 100 in integer arithmetic.
  - `--percentiles` and `--cutoffs` (1–240 minutes) must be **strictly increasing
    and unique**. `route --r5r-out`, `r5r-reduce` and `r5r_reference.py` reject
    anything else, for example `100,7` or `7,7`. Each percentile is evaluated on
    its own. With `--opportunities`, a non-empty `--cutoffs` list is required;
    all three reject an omitted or empty (`""`) one (exit 2).
  - Floating-point evaluation drifts on exact boundaries: for n = 100 it misranks
    p = 7, 14, 28, 55 and 56. Unreached means
  later than `--max-travel-minutes`, as r5r's `max_trip_duration`. Times are whole
  Nigiri minutes; R5 works in seconds and truncates to minutes, which gives the same
  values for whole minutes.
- **Accessibility** (like `r5r::accessibility()`). For each opportunity column,
  percentile and cutoff, the sum over destinations of opportunities × weight. The
  weight uses the percentile travel time (as seconds) and R5's decay functions:
  - `step`: 1 if t < cutoff;
  - `linear:W` (width W minutes): R5's per-second table over cutoff ± W/2;
  - `exponential`: half-life = cutoff;
  - `fixed-exponential:K`: exp(−K·t), with K per second;
  - `logistic:SD` (SD in minutes).

  Opportunities beyond R5's zero point are skipped: the bisection to weight < 0.001
  for the asymptotic functions. R5 evaluates `exp` and `log` with FastMath, so values
  can differ from `std::exp` in the last bit; for `step` and `linear` the definitions
  are exact. Cutoffs and percentiles must be ascending.
- **Files:**
  - `ttm.bin`: magic `VRR5TTM1`, `u32` version 1, n_dests, n_percentiles,
    n_departures, `f64` percentiles[]. Then, per origin in completion order: `u32`
    origin and `u16` minutes [n_dests][n_percentiles], with 65535 for unreached.
  - `accessibility.csv`: `origin,opportunity,percentile,cutoff,accessibility`.
- **`motis-accessibility r5r-reduce --stream S.bin --r5r-out DIR ...`** computes
  the same two files from a raw `route --out` stream, with the same options. It is
  the output-equivalent "raw stream + reducer" path.
- **`motis-accessibility r5r-selftest --out DIR`** checks the percentile ranks
  against hand-ranked boundaries: times 1..N, every p in 1..100, N from 1 to 1440.
- **Stream reading:** `r5r-reduce` and `r5r_reference.py` require complete records,
  including the optional transfer and egress-stop blocks. They reject a truncated
  record or trailing bytes after the last record (exit 2).
- **`r5r_test.py TMP [motis-accessibility]`** runs regression tests on a synthetic
  stream: rejected percentile lists, an omitted or empty `--cutoffs` with
  `--opportunities`, a stream missing its final byte, and a stream with a trailing
  byte, plus, with the binary, `r5r-reduce` against the reference.
- **Reference check:** `r5r_reference.py --stream S.bin --r5r-dir DIR ...` recomputes
  both files from a raw `--out` stream of the same run, using only the standard library.

## Stream format (`VRTTMATX`, version 1, little-endian)

The header is 56 bytes:

- `char[8]` magic
- `u32` version
- `u32` n_origins_total, `u32` n_dests, `u32` n_departures
- `u32` origin_from, `u32` origin_to
- `u16` max_travel_minutes
- `u8` flags (bit 0: transfers block; bit 1: egress-stop block; **bit 2: u16 travel
  times**)
- `u8` target (0 = destinations, 1 = stops)
- `u32[5]` reserved

It is followed by `n_departures × i64` departure times (UTC unix minutes). Records
come in any order. Each record is:

- `u32` origin
- travel times `[dep][dest]`: `u8` with 0..CAP and **255** = unreachable when CAP ≤
  253; `u16` with **65535** = unreachable when CAP > 253 (flag bit 2)
- optional `u8` transfers `[dep][dest]`:
  - destinations: 0..14 transfers, 253 walk via stop, 254 direct walk, 255
    unreachable;
  - stops: k = 0..15 vehicles, 255 unreachable.
- optional `u32` egress_stop `[dep][dest]`: 0xFFFFFFFF = none

The payload is arrival times only. **No journeys are reconstructed.**

### Sparse records (`--stream-encoding sparse`, version 2)

The default (`dense`) writes version 1 as above. `--stream-encoding sparse` writes
the same header with **version 2** and the same departures. Each record is then:

- `u32` origin
- `u8` encoding: 0 = dense, 1 = bitmap
- dense: the version-1 record payload, unchanged (travel times, then the optional
  transfer and egress-stop blocks)
- bitmap, over the record's C = n_departures × n_dests cells in `[dep][dest]` order:
  - `u32` R, the number of reached cells (travel time not unreachable)
  - presence bitmap, ⌈C / 8⌉ bytes: cell i is bit (i mod 8) of byte ⌊i / 8⌋ (least
    significant bit first); padding bits are 0
  - travel times of the R reached cells in cell order (`u8`, or `u16` with flag bit 2)
  - with flag bit 0: their `u8` transfer codes; with flag bit 1: their `u32` egress
    stops

The writer picks the bitmap form when it is smaller than the dense payload.
Unreached cells always hold 255/65535, transfer code 255 and egress stop
0xFFFFFFFF, so the dense record is recovered exactly. `motis-accessibility
stream-decode --in S2.bin --out S1.bin` rewrites a version-2 stream as the
version-1 stream (version field 1, records in the same order). The other readers
(`r5r-reduce`, `r5r_reference.py`, `vr-access`) accept version 1 only and reject
version 2 with an error. `stream_encoding` and `bitmap_records` are in the timing
JSON.

## `vr-access`

| Command | Purpose |
|---|---|
| `departures` | Converts a local window to UTC minutes (chrono-tz). |
| `straight-connectors`, `straight-direct` | Haversine distance (R = 6,371 km), `ceil` at the given speed, inclusive radius. Only stops with traffic. |
| `stop-origins` | Every stop with traffic as an origin, with an identity access connector. |
| `reduce` | Population-weighted cumulative accessibility per origin; see `src/reduce.rs`. Semantics: `fixed`, `best`, `meancount`, `meantt`, `meanttcap`. Options: `--origins`, `--exclude-self` (by `cell_id`), `--continuous`. |
| `compare` | Exact typed comparison. Fails on any travel-time difference, duplicate origins in A, or origins missing from A. With `--strict-transfers` it also requires both sides to carry transfers and to agree on every reachable cell. |
| `validate` | Output gate: header and value invariants, unique and expected origins, transfer codes, consistency with the direct-walk file, and a canonical, thread-order-independent digest. |
| `motis-queries`, `from-motis-batch` | Stock control for grids (`/api/experimental/one-to-many-intermodal`). |
| `motis-one-to-all-queries`, `from-motis-one-to-all` | Stock control for stops (`/api/v6/one-to-all`). |
| `stop-summary` | Per origin × departure: reachable active stops, sum of travel times and digest. |
| `oracle` | Independent GTFS connection scan; it shares only the stop index, transfer times and footpaths with Nigiri. It honours `transfer_type=3` trip-to-trip rules. Semantics: `stock`, `strict`, `walkstop`. Has `--explain` witnesses and `--active-stops-out`. The full rule list is in `src/oracle.rs`. |
| `connector-compare`, `summary-diff` | Paired comparison, e.g. straight-line vs OSM arm. |
| `inspect`, `dump` | Stream header and counts; CSV dump. |
