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
motis-accessibility route      --data DIR --n-origins N [--n-dests M] --access A.csv [--egress E.csv] [--direct D.csv] \
                               --dep-start-unix-minutes T0 [--dep-count C] [--dep-step-minutes S] --max-travel-minutes CAP \
                               [--max-transfers 0..14] [--target dests|stops] [--egress-rounds transit|all] \
                               [--origin-from I --origin-to J | --origin-list I,J,..] [--with-transfers] [--with-egress-stop] \
                               [--threads T] [--pin-threads] [--reuse-state] [--discard] --out stream.bin [--timing-json t.json]
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
- **Stops** (`--target stops`): the fastest arrival at every location, including
  round 0, as in stock `/api/v6/one-to-all`. Here the transfers block holds **k**,
  the number of vehicles used (0 = start or walk only), not k − 1.

Bounds and modes:

- `--max-transfers` must be in **[0, 14]**, Nigiri's `kMaxTransfers`. Values outside
  the range are rejected before narrowing.
- `--max-travel-minutes` must be in **[0, 1440]**.
- `--discard` computes everything and writes no records. It is for timing only.
- `--reuse-state` keeps one Nigiri `raptor_state` per worker thread instead of a
  fresh one per query; every query still starts from a reset state, so outputs are
  unchanged. With a Nigiri that provides `one_to_all(tt, rtt, q, raptor_state&)` the
  stock function runs into the reused state; otherwise the tool runs its body.
- `--pin-threads` pins worker *i* to the *i*-th CPU the process may use (its
  cpuset). The timing JSON records the allowed CPUs.
- The timing JSON records the effective `max_transfers`, `max_travel_minutes`,
  `egress_rounds`, `discard` and the stage times.

Nigiri `one_to_all` keeps a round's arrival at a stop only if it is strictly earlier
than all earlier rounds. With stock egress (k ≥ 1 only), a stop reachable on foot
from the origin no later than by transit therefore offers no transit egress.

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
