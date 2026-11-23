//! Reference station kernel on a `motis-accessibility export-tt` directory: a round-based
//! connection scan (exact earliest arrival with <= K transfers) that mirrors the pinned
//! Nigiri one_to_all semantics documented in the export schema. Used to prove that the
//! export is complete and its semantics are stated correctly (it must equal the candidate
//! `route --target stops` stream exactly), and as an executable spec for other kernels.

use crate::geo::{read_connectors, Table};
use crate::stream::{Header, Record, Writer, FLAG_TRANSFERS, TR_UNREACHABLE, UNREACHABLE};
use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};

#[derive(clap::Args)]
pub struct ExportOracleArgs {
    #[arg(long)]
    export: PathBuf,
    /// identity (or any) access connectors point,stop,minutes
    #[arg(long)]
    access: PathBuf,
    #[arg(long)]
    n_origins: u32,
    #[arg(long, value_delimiter = ',')]
    sample: Vec<u32>,
    #[arg(long)]
    dep_start_unix_minutes: i64,
    #[arg(long)]
    max_travel: u16,
    #[arg(long, default_value_t = 14)]
    max_transfers: u8,
    #[arg(long)]
    out: PathBuf,
}

struct Conn {
    from: u32,
    to: u32,
    dep: i64,
    arr: i64,
    trip: u32,
    pickup: bool,
    dropoff: bool,
}

pub struct Tt {
    n_loc: usize,
    base: Vec<u32>,
    transfer: Vec<i64>,
    fps: Vec<Vec<(u32, i64)>>,
    conns: Vec<Conn>,
    n_trips: usize,
    pub stats: serde_json::Value,
}

pub fn load(dir: &Path) -> Result<Tt> {
    let lt = Table::read(&dir.join("locations.csv"))?;
    let (cl, cb, ctt) = (lt.col("loc")?, lt.col("base")?, lt.col("transfer_time")?);
    let n_loc = lt.rows.len();
    let mut base = vec![0u32; n_loc];
    let mut transfer = vec![0i64; n_loc];
    for (i, r) in lt.rows.iter().enumerate() {
        if r[cl].parse::<usize>()? != i {
            bail!("locations.csv not dense at row {}", i);
        }
        base[i] = r[cb].parse()?;
        transfer[i] = r[ctt].parse()?;
    }
    let ft = Table::read(&dir.join("footpaths.csv"))?;
    let mut fps = vec![Vec::new(); n_loc];
    for r in &ft.rows {
        fps[r[0].parse::<usize>()?].push((r[1].parse()?, r[2].parse()?));
    }
    // route stop sequences
    let rs = Table::read(&dir.join("route_stops.csv"))?;
    let mut routes: Vec<Vec<(u32, bool, bool)>> = Vec::new();
    for r in &rs.rows {
        let (ri, seq): (usize, usize) = (r[0].parse()?, r[1].parse()?);
        if routes.len() <= ri {
            routes.resize(ri + 1, Vec::new());
        }
        if routes[ri].len() != seq {
            bail!("route_stops.csv: route {} seq {} out of order", ri, seq);
        }
        routes[ri].push((r[2].parse()?, &r[3] == "1", &r[4] == "1"));
    }
    let tr = Table::read(&dir.join("trips.csv"))?;
    let trip_route: Vec<usize> = tr.rows.iter().map(|r| r[1].parse::<usize>()).collect::<std::result::Result<_, _>>()?;
    let ev = Table::read(&dir.join("events.csv"))?;
    let mut conns = Vec::new();
    let mut prev: Option<(u32, usize, i64)> = None; // (trip, seq, dep)
    let (mut bad_order, mut n_ev) = (0u64, 0u64);
    for r in &ev.rows {
        n_ev += 1;
        let (trip, seq): (u32, usize) = (r[0].parse()?, r[1].parse()?);
        let (arr, dep): (i64, i64) = (r[2].parse()?, r[3].parse()?);
        let route = &routes[trip_route[trip as usize]];
        if seq >= route.len() {
            bail!("events.csv: trip {} seq {} beyond route length", trip, seq);
        }
        if arr >= 0 && dep >= 0 && dep < arr {
            bad_order += 1;
        }
        if let Some((pt, ps, pdep)) = prev {
            if pt == trip {
                if ps + 1 != seq {
                    bail!("events.csv: trip {} seq gap", trip);
                }
                if arr < pdep {
                    bad_order += 1;
                }
                conns.push(Conn { from: route[ps].0, to: route[seq].0, dep: pdep, arr, trip, pickup: route[ps].1, dropoff: route[seq].2 });
            }
        }
        prev = Some((trip, seq, dep));
    }
    conns.sort_by_key(|c| (c.dep, c.arr));
    let stats = serde_json::json!({"locations": n_loc, "footpaths": ft.rows.len(), "routes": routes.len(), "trips": tr.rows.len(), "events": n_ev, "connections": conns.len(), "events_out_of_order": bad_order});
    Ok(Tt { n_loc, base, transfer, fps, conns, n_trips: tr.rows.len(), stats })
}

pub fn run(a: ExportOracleArgs) -> Result<()> {
    let t = load(&a.export).with_context(|| format!("load export {}", a.export.display()))?;
    if a.max_transfers > 14 {
        bail!("--max-transfers must be in [0, 14]");
    }
    let mut acc: Vec<Vec<(u32, i64)>> = vec![vec![]; a.n_origins as usize];
    for (p, s, m) in read_connectors(&a.access)? {
        acc[p as usize].push((s, m as i64));
    }
    let n = t.n_loc;
    let header = Header { n_origins_total: a.n_origins, n_dests: n as u32, n_departures: 1, origin_from: 0, origin_to: a.n_origins, max_travel: a.max_travel, flags: FLAG_TRANSFERS, target: 1, departures: vec![a.dep_start_unix_minutes] };
    let mut w = Writer::create(&a.out, header)?;
    const INF: i64 = i64::MAX / 4;
    let (t0, cap) = (a.dep_start_unix_minutes, a.max_travel as i64);
    let start = t.conns.partition_point(|c| c.dep < t0);
    let end = t.conns.partition_point(|c| c.dep <= t0 + cap);
    let mut boarded = vec![u32::MAX; t.n_trips]; // round in which boarded
    let t1 = std::time::Instant::now();
    for &o in &a.sample {
        let mut ready = vec![INF; n];
        let mut out = vec![INF; n];
        let mut out_k = vec![TR_UNREACHABLE; n];
        for &(s, m) in &acc[o as usize] {
            let x = t0 + m;
            if x < ready[s as usize] {
                ready[s as usize] = x;
            }
            if t.base[s as usize] == s && x - t0 <= cap && x < out[s as usize] {
                out[s as usize] = x;
                out_k[s as usize] = 0;
            }
        }
        boarded.iter_mut().for_each(|b| *b = u32::MAX);
        for k in 1..=(a.max_transfers as u32 + 1) {
            let mut next = ready.clone();
            let mut any = false;
            for c in &t.conns[start..end] {
                let ti = c.trip as usize;
                if boarded[ti] != k {
                    if c.pickup && ready[c.from as usize] <= c.dep {
                        boarded[ti] = k;
                    } else {
                        continue;
                    }
                }
                if !c.dropoff || c.arr - t0 > cap {
                    continue;
                }
                let to = c.to as usize;
                let b = t.base[to] as usize;
                if c.arr < out[b] {
                    out[b] = c.arr;
                    out_k[b] = k as u8;
                }
                let r = c.arr + t.transfer[to];
                if r < next[to] {
                    next[to] = r;
                    any = true;
                }
                for &(f, d) in &t.fps[to] {
                    let x = c.arr + d;
                    if x - t0 > cap {
                        continue;
                    }
                    if x < next[f as usize] {
                        next[f as usize] = x;
                        any = true;
                    }
                    if t.base[f as usize] == f && x < out[f as usize] {
                        out[f as usize] = x;
                        out_k[f as usize] = k as u8;
                    }
                }
            }
            ready = next;
            if !any {
                break;
            }
        }
        let tt: Vec<u16> = out.iter().map(|&x| if x >= INF { UNREACHABLE } else { (x - t0) as u16 }).collect();
        let tr: Vec<u8> = out.iter().zip(&out_k).map(|(&x, &k)| if x >= INF { TR_UNREACHABLE } else { k }).collect();
        w.write(&Record { origin: o, tt, transfers: tr, via: vec![] })?;
    }
    w.finish()?;
    println!("{}", serde_json::to_string_pretty(&serde_json::json!({"tool": "vr-access export-oracle", "export": t.stats, "origins": a.sample.len(), "max_transfers": a.max_transfers, "max_travel": a.max_travel, "connections_in_window": end - start, "scan_seconds": t1.elapsed().as_secs_f64()}))?);
    Ok(())
}
