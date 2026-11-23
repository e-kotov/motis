mod compare;
mod geo;
mod motis_batch;
mod oracle;
mod paired;
mod reduce;
mod stops_mode;
mod stream;

use anyhow::{bail, Context, Result};
use chrono::{NaiveDate, NaiveTime, TimeZone};
use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "vr-access", version, about = "Offline accessibility toolkit for the MOTIS accessibility batch")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Departure grid: local wall-clock window -> UTC unix minutes (prints start, count, step).
    Departures {
        #[arg(long)]
        date: NaiveDate,
        #[arg(long)]
        timezone: String,
        #[arg(long)]
        from: NaiveTime,
        /// exclusive end, e.g. 10:00 for 07:30..09:59
        #[arg(long)]
        to: NaiveTime,
        #[arg(long, default_value_t = 1)]
        step: i64,
    },
    /// Straight-line point->stop connectors (access == egress by symmetry).
    StraightConnectors {
        #[arg(long)]
        points: PathBuf,
        #[arg(long)]
        stops: PathBuf,
        #[arg(long, default_value_t = 2000.0)]
        max_m: f64,
        #[arg(long, default_value_t = 5000.0 / 3600.0)]
        speed_mps: f64,
        /// also connect stops without any route (default: only stops with traffic, as stock MOTIS)
        #[arg(long)]
        all_stops: bool,
        #[arg(long)]
        out: PathBuf,
    },
    /// Use every stop with traffic as an origin (kernel / historical scaling workloads):
    /// writes points.csv (lat,lon,stop) and access connectors point -> own stop, 0 minutes.
    StopOrigins {
        #[arg(long)]
        stops: PathBuf,
        #[arg(long)]
        points: PathBuf,
        #[arg(long)]
        access: PathBuf,
    },
    /// Straight-line origin->destination direct walks.
    StraightDirect {
        #[arg(long)]
        origins: PathBuf,
        #[arg(long)]
        dests: PathBuf,
        #[arg(long, default_value_t = 2000.0)]
        max_m: f64,
        #[arg(long, default_value_t = 5000.0 / 3600.0)]
        speed_mps: f64,
        #[arg(long)]
        out: PathBuf,
    },
    /// Population-weighted cumulative accessibility per origin (CSV + JSON summary).
    Reduce {
        #[arg(long, required = true, num_args = 1..)]
        stream: Vec<PathBuf>,
        #[arg(long)]
        weights: PathBuf,
        #[arg(long, default_value = "pop")]
        weight_col: String,
        #[arg(long, default_value_t = 0)]
        fixed_index: usize,
        #[arg(long, value_delimiter = ',', default_value = "1000,5000,10000,30000,50000")]
        cumulative: Vec<f64>,
        #[arg(long, default_value_t = 4)]
        threads: usize,
        /// origin cells (cell_id,lat,lon); default: the weights file (origins == destinations)
        #[arg(long)]
        origins: Option<PathBuf>,
        /// do not count the destination cell with the origin's cell_id (or the same index)
        #[arg(long)]
        exclude_self: bool,
        /// use continuous straight-line direct-walk minutes (from lat/lon) in t_N / meantt*
        #[arg(long)]
        continuous: bool,
        #[arg(long, default_value_t = 5000.0 / 3600.0)]
        continuous_speed_mps: f64,
        #[arg(long, default_value_t = 2000.0)]
        continuous_max_m: f64,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        json: Option<PathBuf>,
    },
    /// Compare stream A (streamed) against stream B (loaded; the sampled side).
    Compare {
        #[arg(long)]
        a: PathBuf,
        #[arg(long)]
        b: PathBuf,
        #[arg(long, default_value_t = 20)]
        max_report: usize,
        /// apply this inclusive cap to both sides before comparing (default: min header cap)
        #[arg(long)]
        cap: Option<u16>,
        #[arg(long)]
        json: Option<PathBuf>,
        /// also fail when transfer counts differ
        #[arg(long)]
        strict_transfers: bool,
    },
    /// Generate stock `motis batch` one-to-many-intermodal queries for sampled origins.
    MotisQueries {
        #[arg(long)]
        origins: PathBuf,
        #[arg(long)]
        dests: PathBuf,
        /// comma-separated origin indices
        #[arg(long, value_delimiter = ',')]
        sample: Vec<u32>,
        #[arg(long)]
        dep_start_unix_minutes: i64,
        #[arg(long, default_value_t = 1)]
        dep_count: i64,
        #[arg(long, default_value_t = 1)]
        dep_step: i64,
        #[arg(long)]
        max_travel: u16,
        #[arg(long)]
        walk_seconds: u32,
        #[arg(long, default_value_t = 5000.0 / 3600.0)]
        pedestrian_speed: f64,
        #[arg(long, default_value_t = 25.0)]
        max_matching_distance: f64,
        #[arg(long, default_value_t = 1000)]
        chunk: usize,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        map: PathBuf,
    },
    /// Convert stock `motis batch` responses into a VRTT stream.
    FromMotisBatch {
        #[arg(long)]
        map: PathBuf,
        #[arg(long)]
        responses: PathBuf,
        #[arg(long)]
        n_origins: u32,
        #[arg(long)]
        n_dests: u32,
        #[arg(long)]
        dep_start_unix_minutes: i64,
        #[arg(long, default_value_t = 1)]
        dep_count: i64,
        #[arg(long, default_value_t = 1)]
        dep_step: i64,
        #[arg(long)]
        max_travel: u16,
        #[arg(long)]
        out: PathBuf,
    },
    /// Dump a stream as CSV: origin,departure_index,dest,tt,transfers,egress_stop
    /// (tt/transfers empty when unreachable/absent).
    Dump {
        #[arg(long)]
        stream: PathBuf,
    },
    /// Station-to-station summary of a stops stream (per origin x departure): reachable
    /// active stops (denominator = --active list), their travel-time sum and a digest.
    StopSummary {
        #[arg(long)]
        stream: PathBuf,
        /// CSV with a `stop` column (e.g. `oracle --active-stops-out`)
        #[arg(long)]
        active: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Stock control for stop origins: `motis batch` queries for /api/v6/one-to-all.
    MotisOneToAllQueries {
        #[arg(long)]
        stops: PathBuf,
        /// CSV with a `stop` column; row order = origin index
        #[arg(long)]
        origins: PathBuf,
        /// dataset tag used at import (stop ids are "<tag>_<gtfs id>")
        #[arg(long)]
        tag: String,
        #[arg(long)]
        dep_start_unix_minutes: i64,
        #[arg(long, default_value_t = 1)]
        dep_count: i64,
        #[arg(long, default_value_t = 1)]
        dep_step: i64,
        #[arg(long)]
        max_travel: u16,
        #[arg(long, default_value_t = 0)]
        pre_seconds: u32,
        /// stock maxTransfers (Nigiri semantics: up to max_transfers + 1 vehicles)
        #[arg(long)]
        max_transfers: Option<u8>,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        map: PathBuf,
    },
    /// Convert stock one-to-all responses to a stops stream.
    FromMotisOneToAll {
        #[arg(long)]
        map: PathBuf,
        #[arg(long)]
        responses: PathBuf,
        #[arg(long)]
        stops: PathBuf,
        #[arg(long)]
        tag: String,
        #[arg(long)]
        n_origins: u32,
        #[arg(long)]
        dep_start_unix_minutes: i64,
        #[arg(long, default_value_t = 1)]
        dep_count: i64,
        #[arg(long, default_value_t = 1)]
        dep_step: i64,
        #[arg(long)]
        max_travel: u16,
        #[arg(long)]
        out: PathBuf,
    },
    /// Paired connector comparison (e.g. straight vs OSM access): identity and minutes.
    ConnectorCompare {
        #[arg(long)]
        a: PathBuf,
        #[arg(long)]
        b: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Paired per-origin accessibility difference (B - A) between two reduce outputs.
    SummaryDiff {
        #[arg(long)]
        a: PathBuf,
        #[arg(long)]
        b: PathBuf,
        /// origin cells (weights for population-weighted totals)
        #[arg(long)]
        origins: PathBuf,
        #[arg(long, default_value = "pop")]
        weight_col: String,
        #[arg(long, default_value = "fixed")]
        semantics: String,
        #[arg(long, value_delimiter = ',', default_value = "30,45,60,95")]
        t: Vec<usize>,
        #[arg(long, value_delimiter = ',', default_value = "1000,5000,10000,30000,50000")]
        levels: Vec<f64>,
        #[arg(long)]
        out: PathBuf,
    },
    /// Print a stream header and per-departure reachability counts.
    Inspect {
        #[arg(long)]
        stream: PathBuf,
    },
    /// Independent time-expanded connection-scan oracle from GTFS (see oracle.rs).
    Oracle(oracle::OracleArgs),
}

fn sha256_file(p: &PathBuf) -> Result<String> {
    let mut f = std::fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}

fn departures(start: i64, count: i64, step: i64) -> Vec<i64> {
    (0..count).map(|i| start + i * step).collect()
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Departures { date, timezone, from, to, step } => {
            let tz: chrono_tz::Tz = timezone.parse().map_err(|e| anyhow::anyhow!("{e}"))?;
            let a = tz.from_local_datetime(&date.and_time(from)).single().context("ambiguous start")?;
            let b = tz.from_local_datetime(&date.and_time(to)).single().context("ambiguous end")?;
            let (s, e) = (a.timestamp() / 60, b.timestamp() / 60);
            if e <= s {
                bail!("empty window");
            }
            let count = (e - s + step - 1) / step;
            println!("{{\"dep_start_unix_minutes\":{},\"dep_count\":{},\"dep_step_minutes\":{},\"first_utc\":\"{}\",\"last_utc\":\"{}\"}}", s, count, step, motis_batch::iso(s), motis_batch::iso(s + (count - 1) * step));
        }
        Cmd::StraightConnectors { points, stops, max_m, speed_mps, all_stops, out } => {
            let p = geo::read_points(&points)?;
            let st = geo::read_stops(&stops)?;
            let pos: Vec<geo::LatLon> = st.iter().map(|s| s.pos).collect();
            let ok = |j: u32| all_stops || st[j as usize].traffic;
            let rows = geo::straight_pairs(&p, &pos, &ok, max_m, speed_mps);
            geo::write_connectors(&out, &rows)?;
            let covered = rows.iter().map(|r| r.0).collect::<std::collections::HashSet<_>>().len();
            eprintln!("straight-connectors: points={} stops={} pairs={} points_with_stop={} sha256={}", p.len(), st.len(), rows.len(), covered, sha256_file(&out)?);
        }
        Cmd::StopOrigins { stops, points, access } => {
            let st = geo::read_stops(&stops)?;
            let mut w = std::io::BufWriter::new(std::fs::File::create(&points)?);
            use std::io::Write as _;
            writeln!(w, "lat,lon,stop")?;
            let mut rows = Vec::new();
            for s in st.iter().filter(|s| s.traffic) {
                writeln!(w, "{:.7},{:.7},{}", s.pos.lat, s.pos.lon, s.idx)?;
                rows.push((rows.len() as u32, s.idx, 0u16));
            }
            w.flush()?;
            geo::write_connectors(&access, &rows)?;
            eprintln!("stop-origins: {} origins", rows.len());
        }
        Cmd::StraightDirect { origins, dests, max_m, speed_mps, out } => {
            let o = geo::read_points(&origins)?;
            let d = geo::read_points(&dests)?;
            let ok = |_j: u32| true;
            let rows = geo::straight_pairs(&o, &d, &ok, max_m, speed_mps);
            geo::write_connectors(&out, &rows)?;
            eprintln!("straight-direct: origins={} dests={} pairs={} sha256={}", o.len(), d.len(), rows.len(), sha256_file(&out)?);
        }
        Cmd::Reduce { stream, weights, weight_col, fixed_index, cumulative, threads, origins, exclude_self, continuous, continuous_speed_mps, continuous_max_m, out, json } => {
            let w = geo::read_weights(&weights, &weight_col)?;
            let t0 = std::time::Instant::now();
            let opath = origins.clone().unwrap_or_else(|| weights.clone());
            let dest_pts = geo::read_points(&weights)?;
            let orig_pts = geo::read_points(&opath)?;
            let self_index = if exclude_self {
                let dt = geo::Table::read(&weights)?;
                let ot = geo::Table::read(&opath)?;
                match (dt.col("cell_id"), ot.col("cell_id")) {
                    (Ok(dc), Ok(oc)) => {
                        let m: std::collections::HashMap<&str, usize> = dt.rows.iter().enumerate().map(|(i, r)| (r.get(dc).unwrap(), i)).collect();
                        Some(ot.rows.iter().map(|r| m.get(r.get(oc).unwrap()).copied()).collect::<Vec<_>>())
                    }
                    _ => {
                        if orig_pts.len() != dest_pts.len() {
                            bail!("--exclude-self without cell_id columns needs origins == destinations");
                        }
                        Some((0..orig_pts.len()).map(Some).collect())
                    }
                }
            } else {
                None
            };
            let n_self = self_index.as_ref().map(|v| v.iter().filter(|x| x.is_some()).count()).unwrap_or(0);
            let cont = if continuous { Some(reduce::Continuous { origins: &orig_pts, dests: &dest_pts, grid: geo::Grid::new(&dest_pts, 0.05), speed_mps: continuous_speed_mps, max_m: continuous_max_m }) } else { None };
            let p = reduce::Params { weights: &w, fixed_index, cumulative: &cumulative, threads, self_index, continuous: cont };
            let s = reduce::reduce_streams(&stream, &p)?;
            reduce::write_csv(&s, &cumulative, &out)?;
            let secs = t0.elapsed().as_secs_f64();
            let total_w: f64 = w.iter().sum();
            let mean_c = |k: usize, t: usize| -> f64 {
                if s.results.is_empty() {
                    return 0.0;
                }
                s.results.iter().map(|r| r.cum[k][t]).sum::<f64>() / s.results.len() as f64
            };
            let summary = serde_json::json!({
                "tool": "vr-access reduce",
                "streams": stream.iter().map(|p| p.display().to_string()).collect::<Vec<_>>(),
                "records": s.records,
                "origins": s.results.len(),
                "n_dests": s.n_dests,
                "n_departures": s.n_dep,
                "cap_minutes": s.cap,
                "weights_total": total_w,
                "fixed_index": fixed_index,
                "exclude_self": exclude_self,
                "origins_with_self_cell": n_self,
                "origins_file": opath.display().to_string(),
                "continuous_direct": if continuous { serde_json::json!({"speed_mps": continuous_speed_mps, "max_m": continuous_max_m}) } else { serde_json::Value::Null },
                "mean_over_origins_c_cap": reduce::SEMANTICS.iter().enumerate().map(|(k, n)| (n.to_string(), serde_json::json!(mean_c(k, s.cap)))).collect::<serde_json::Map<_, _>>(),
                "reachable_pairs_fixed": s.results.iter().map(|r| r.reachable_pairs_fixed).sum::<u64>(),
                "reduce_seconds": secs,
                "out": out.display().to_string(),
                "out_sha256": sha256_file(&out)?,
            });
            println!("{}", serde_json::to_string_pretty(&summary)?);
            if let Some(j) = json {
                std::fs::write(j, serde_json::to_string_pretty(&summary)?)?;
            }
        }
        Cmd::Compare { a, b, max_report, cap, json, strict_transfers } => {
            let rep = compare::compare(&a, &b, max_report, cap)?;
            let txt = serde_json::to_string_pretty(&rep)?;
            println!("{}", txt);
            if let Some(j) = json {
                std::fs::write(j, &txt)?;
            }
            if !rep.tt_identical() {
                eprintln!("compare: travel times DIFFER");
                std::process::exit(3);
            }
            if strict_transfers && rep.transfers_equal != rep.transfers_compared {
                eprintln!("compare: transfer counts DIFFER");
                std::process::exit(4);
            }
            eprintln!("compare: travel times identical ({} cells, {} reachable)", rep.cells_compared, rep.reachable_equal);
        }
        Cmd::MotisQueries { origins, dests, sample, dep_start_unix_minutes, dep_count, dep_step, max_travel, walk_seconds, pedestrian_speed, max_matching_distance, chunk, out, map } => {
            let o = geo::read_points(&origins)?;
            let d = geo::read_points(&dests)?;
            let q = motis_batch::QueryParams { max_travel, pre_post_direct_seconds: walk_seconds, pedestrian_speed, max_matching_distance, chunk };
            let n = motis_batch::write_queries(&o, &sample, &d, &departures(dep_start_unix_minutes, dep_count, dep_step), &q, &out, &map)?;
            eprintln!("motis-queries: {} queries", n);
        }
        Cmd::FromMotisBatch { map, responses, n_origins, n_dests, dep_start_unix_minutes, dep_count, dep_step, max_travel, out } => {
            let info = motis_batch::convert_responses(&map, &responses, n_origins, n_dests, &departures(dep_start_unix_minutes, dep_count, dep_step), max_travel, &out)?;
            println!("{}", serde_json::to_string_pretty(&info)?);
        }
        Cmd::StopSummary { stream, active, out } => {
            let act = stops_mode::read_stop_list(&active)?;
            let info = stops_mode::stop_summary(&stream, &act, &out)?;
            println!("{}", serde_json::to_string_pretty(&info)?);
        }
        Cmd::MotisOneToAllQueries { stops, origins, tag, dep_start_unix_minutes, dep_count, dep_step, max_travel, pre_seconds, max_transfers, out, map } => {
            let o = stops_mode::read_stop_list(&origins)?;
            let n = stops_mode::one_to_all_queries(&stops, &o, &tag, &departures(dep_start_unix_minutes, dep_count, dep_step), max_travel, pre_seconds, max_transfers, &out, &map)?;
            eprintln!("motis-one-to-all-queries: {} queries", n);
        }
        Cmd::FromMotisOneToAll { map, responses, stops, tag, n_origins, dep_start_unix_minutes, dep_count, dep_step, max_travel, out } => {
            let info = stops_mode::from_one_to_all(&map, &responses, &stops, &tag, n_origins, &departures(dep_start_unix_minutes, dep_count, dep_step), max_travel, &out)?;
            println!("{}", serde_json::to_string_pretty(&info)?);
        }
        Cmd::ConnectorCompare { a, b, out } => {
            println!("{}", serde_json::to_string_pretty(&paired::connector_compare(&a, &b, &out)?)?);
        }
        Cmd::SummaryDiff { a, b, origins, weight_col, semantics, t, levels, out } => {
            println!("{}", serde_json::to_string_pretty(&paired::summary_diff(&a, &b, &origins, &weight_col, &semantics, &t, &levels, &out)?)?);
        }
        Cmd::Dump { stream } => {
            let (h, recs) = stream::Reader::read_all(&stream)?;
            println!("origin,departure_index,dest,tt,transfers,egress_stop");
            let nd = h.n_dests as usize;
            for r in &recs {
                for i in 0..r.tt.len() {
                    let tt = if r.tt[i] == stream::UNREACHABLE { String::new() } else { r.tt[i].to_string() };
                    let tr = r.transfers.get(i).map(|t| if *t == stream::TR_UNREACHABLE { String::new() } else { t.to_string() }).unwrap_or_default();
                    let via = r.via.get(i).map(|v| if *v == stream::NO_STOP { String::new() } else { v.to_string() }).unwrap_or_default();
                    println!("{},{},{},{},{},{}", r.origin, i / nd, i % nd, tt, tr, via);
                }
            }
        }
        Cmd::Inspect { stream } => {
            let mut rd = stream::Reader::open(&stream)?;
            let h = rd.header.clone();
            println!("{:?}", h);
            let mut rec = stream::Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
            let mut n = 0u64;
            let mut reach = vec![0u64; h.n_departures as usize];
            let mut origins = Vec::new();
            while rd.next_into(&mut rec)? {
                n += 1;
                origins.push(rec.origin);
                for di in 0..h.n_departures as usize {
                    reach[di] += rec.tt[di * h.n_dests as usize..(di + 1) * h.n_dests as usize].iter().filter(|&&t| t != stream::UNREACHABLE).count() as u64;
                }
            }
            origins.sort_unstable();
            println!("records={} first_origins={:?} reachable_per_departure={:?} sha256={}", n, &origins[..origins.len().min(10)], reach, sha256_file(&stream)?);
        }
        Cmd::Oracle(a) => oracle::run(a)?,
    }
    Ok(())
}
