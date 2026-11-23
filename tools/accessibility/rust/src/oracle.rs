//! Independent earliest-arrival oracle: a connection scan (CSA) over trips read
//! directly from GTFS, sharing only the stop index, per-stop transfer times and the
//! footpath graph exported from the Nigiri timetable (`motis-accessibility stops`).
//! It does not call or reuse any Nigiri/MOTIS routing code.
//!
//! Rules mirrored from the stock contract (each one is a checkable assumption):
//! - GTFS times are relative to local noon minus 12 h of the service day (agency
//!   timezone); seconds are truncated to minutes (as Nigiri's hhmm_to_min).
//! - Services active on D-1 and D are expanded (overnight trips).
//! - Boarding at stop s at departure t needs ready(s) <= t; ready is set by
//!   access (t0 + walk, no buffer), by alighting at s (+ the stop's transfer time),
//!   or by a footpath after alighting (+ footpath duration). No footpath after
//!   access (no start footpaths). Pickup/drop-off types 1 are respected.
//! - Trips sharing a GTFS block_id on the same service day are chained in
//!   first-departure order: staying seated needs no transfer.
//! - Arrival at a stop counts the vehicle arrival (no final buffer) and footpath
//!   arrivals; for `--target stops` it also counts round-0 access arrivals.
//! - `--semantics stock` (dests): a transit arrival at a stop is ignored when it is
//!   not strictly earlier than the access-walk arrival at that stop (Nigiri
//!   one_to_all keeps only strictly improving rounds and stock egress skips round
//!   0). `strict` uses every transit arrival.
//! - Unlimited transfers (CSA); Nigiri stops at kMaxTransfers = 14.
//! - GTFS transfers.txt `transfer_type=3` trip-to-trip rules (from_trip, to_trip,
//!   from_stop, to_stop) forbid boarding the to-trip with readiness that came from the
//!   from-trip alighting at from_stop. Readiness is tracked exactly (eligibility-aware:
//!   one universal label plus every restricted (trip, alighting stop) label not dominated
//!   by it; no cap). Other transfer types are not modelled.

use crate::geo::{read_connectors, Table};
use crate::stream::{Header, Record, Writer, DIRECT_WALK, UNREACHABLE};
use anyhow::{bail, Context, Result};
use chrono::{Datelike, Duration, NaiveDate, NaiveTime, TimeZone};
use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};

#[derive(clap::Args)]
pub struct OracleArgs {
    /// GTFS directory or zip
    #[arg(long)]
    gtfs: PathBuf,
    #[arg(long)]
    date: NaiveDate,
    /// stops.csv exported by `motis-accessibility stops`
    #[arg(long)]
    stops: PathBuf,
    /// footpaths.csv exported by `motis-accessibility stops --footpaths`
    #[arg(long)]
    footpaths: PathBuf,
    #[arg(long)]
    access: PathBuf,
    #[arg(long)]
    egress: Option<PathBuf>,
    #[arg(long)]
    direct: Option<PathBuf>,
    #[arg(long)]
    n_dests: Option<u32>,
    #[arg(long, default_value = "dests")]
    target: String,
    #[arg(long, default_value = "stock")]
    semantics: String,
    #[arg(long)]
    n_origins: u32,
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
    out: PathBuf,
    /// write the active-stop list (stops with >= 1 boardable departure in
    /// [first departure, last departure + max travel]) as CSV `stop,departures`
    #[arg(long)]
    active_stops_out: Option<PathBuf>,
    /// explain cells "origin:departure_index:dest[,..]" (dests target): witness journey
    #[arg(long, value_delimiter = ',')]
    explain: Vec<String>,
}

struct Gtfs {
    files: HashMap<String, Vec<u8>>,
}

impl Gtfs {
    fn open(p: &Path) -> Result<Gtfs> {
        let mut files = HashMap::new();
        if p.is_dir() {
            for e in std::fs::read_dir(p)? {
                let e = e?;
                let n = e.file_name().to_string_lossy().to_string();
                if n.ends_with(".txt") {
                    files.insert(n, std::fs::read(e.path())?);
                }
            }
        } else {
            let mut z = zip::ZipArchive::new(std::fs::File::open(p)?)?;
            for i in 0..z.len() {
                let mut f = z.by_index(i)?;
                let n = f.name().rsplit('/').next().unwrap_or("").to_string();
                if n.ends_with(".txt") {
                    let mut v = Vec::new();
                    f.read_to_end(&mut v)?;
                    files.insert(n, v);
                }
            }
        }
        Ok(Gtfs { files })
    }
    fn table(&self, name: &str) -> Result<Option<(Vec<String>, Vec<csv::StringRecord>)>> {
        let Some(b) = self.files.get(name) else { return Ok(None) };
        let b = b.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(b);
        let mut r = csv::ReaderBuilder::new().flexible(true).from_reader(b);
        let h: Vec<String> = r.headers()?.iter().map(|s| s.trim().to_string()).collect();
        let rows = r.records().collect::<std::result::Result<Vec<_>, _>>()?;
        Ok(Some((h, rows)))
    }
}

fn col(h: &[String], n: &str) -> Option<usize> {
    h.iter().position(|x| x == n)
}

/// minutes since service-day reference (truncated seconds); None if empty.
fn hms(s: &str) -> Option<i64> {
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    let mut it = s.split(':');
    let h: i64 = it.next()?.parse().ok()?;
    let m: i64 = it.next()?.parse().ok()?;
    Some(h * 60 + m)
}

struct Conn {
    from: u32,
    to: u32,
    dep: i64,
    arr: i64,
    trip: u32,
    pickup: bool,
    dropoff: bool,
    last: bool,
}

pub fn run(a: OracleArgs) -> Result<()> {
    let t0 = std::time::Instant::now();
    let g = Gtfs::open(&a.gtfs)?;
    // nigiri stop index
    let st = Table::read(&a.stops)?;
    let (ci, cid, ctt) = (st.col("stop")?, st.col("id")?, st.col("transfer_time")?);
    let n_loc = st.rows.len();
    let mut loc_of: HashMap<String, u32> = HashMap::new();
    let mut transfer_time = vec![0i64; n_loc];
    for r in &st.rows {
        let i: u32 = r[ci].parse()?;
        transfer_time[i as usize] = r[ctt].parse()?;
        if r[cid].is_empty() {
            continue; // Nigiri-generated virtual location (no GTFS stop id)
        }
        if loc_of.insert(r[cid].to_string(), i).is_some() {
            bail!("stop id {} appears in several sources; oracle needs a single dataset", &r[cid]);
        }
    }
    let fpt = Table::read(&a.footpaths)?;
    // Nigiri-generated virtual locations (empty GTFS id) are internal: the oracle neither
    // walks to/from them nor reports arrivals there (Nigiri tracks arrivals at the base).
    let is_virtual: Vec<bool> = st.rows.iter().map(|r| r[cid].is_empty()).collect();
    let mut fps: Vec<Vec<(u32, i64)>> = vec![vec![]; n_loc];
    let mut skipped_virtual_fp = 0u64;
    for r in &fpt.rows {
        let (from, to): (usize, u32) = (r[0].parse()?, r[1].parse()?);
        if is_virtual[from] || is_virtual[to as usize] {
            skipped_virtual_fp += 1;
            continue;
        }
        fps[from].push((to, r[2].parse()?));
    }
    // timezone
    let (ah, ar) = g.table("agency.txt")?.context("agency.txt")?;
    let tzs = ar.first().context("no agency")?[col(&ah, "agency_timezone").context("agency_timezone")?].to_string();
    let tz: chrono_tz::Tz = tzs.parse().map_err(|e| anyhow::anyhow!("{e}"))?;
    // services
    let days = [a.date - Duration::days(1), a.date];
    let mut active: HashMap<String, [bool; 2]> = HashMap::new();
    if let Some((h, rows)) = g.table("calendar.txt")? {
        let wd = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"];
        let (sid, sd, ed) = (col(&h, "service_id").unwrap(), col(&h, "start_date").unwrap(), col(&h, "end_date").unwrap());
        for r in &rows {
            let s = NaiveDate::parse_from_str(r[sd].trim(), "%Y%m%d")?;
            let e = NaiveDate::parse_from_str(r[ed].trim(), "%Y%m%d")?;
            let mut v = [false; 2];
            for (k, d) in days.iter().enumerate() {
                let c = col(&h, wd[d.weekday().num_days_from_monday() as usize]).unwrap();
                v[k] = *d >= s && *d <= e && r[c].trim() == "1";
            }
            active.insert(r[sid].trim().to_string(), v);
        }
    }
    if let Some((h, rows)) = g.table("calendar_dates.txt")? {
        let (sid, dt, ex) = (col(&h, "service_id").unwrap(), col(&h, "date").unwrap(), col(&h, "exception_type").unwrap());
        for r in &rows {
            let d = NaiveDate::parse_from_str(r[dt].trim(), "%Y%m%d")?;
            for (k, dd) in days.iter().enumerate() {
                if d == *dd {
                    active.entry(r[sid].trim().to_string()).or_insert([false; 2])[k] = r[ex].trim() == "1";
                }
            }
        }
    }
    let mut warnings = Vec::new();
    if let Some((_, rows)) = g.table("frequencies.txt")? {
        if !rows.is_empty() {
            warnings.push(format!("frequencies.txt has {} rows (not expanded by the oracle)", rows.len()));
        }
    }
    let mut forb_raw: Vec<(String, String, u32, u32)> = Vec::new();
    if let Some((h, rows)) = g.table("transfers.txt")? {
        if let Some(c) = col(&h, "transfer_type") {
            let n4 = rows.iter().filter(|r| r.get(c).map(|x| x.trim()) == Some("4")).count();
            if n4 > 0 {
                warnings.push(format!("transfers.txt has {} stay-seated (type 4) rows (not modelled)", n4));
            }
            let g_ = |r: &csv::StringRecord, n: &str| col(&h, n).and_then(|i| r.get(i)).map(|x| x.trim().to_string()).unwrap_or_default();
            let mut other = 0u64;
            for r in &rows {
                if r.get(c).map(|x| x.trim()) != Some("3") {
                    if r.get(c).map(|x| x.trim()).unwrap_or("") != "" && r.get(c).map(|x| x.trim()) != Some("0") {
                        other += 1;
                    }
                    continue;
                }
                let (ft, tt_, fs, ts) = (g_(r, "from_trip_id"), g_(r, "to_trip_id"), g_(r, "from_stop_id"), g_(r, "to_stop_id"));
                if ft.is_empty() || tt_.is_empty() {
                    warnings.push("transfers.txt type 3 row without trip ids (not modelled)".into());
                    continue;
                }
                if let (Some(&x), Some(&y)) = (loc_of.get(&fs), loc_of.get(&ts)) {
                    forb_raw.push((ft, tt_, x, y));
                }
            }
            if other > 0 {
                warnings.push(format!("transfers.txt has {} rows of types 1/2/4/5 (not modelled beyond the exported footpaths)", other));
            }
        }
    }
    // trips
    let (th, trows) = g.table("trips.txt")?.context("trips.txt")?;
    let (tid, tsid) = (col(&th, "trip_id").unwrap(), col(&th, "service_id").unwrap());
    let tblk = col(&th, "block_id");
    let mut trip_info: HashMap<String, (String, Option<String>)> = HashMap::new();
    for r in &trows {
        let b = tblk.and_then(|c| r.get(c)).map(|s| s.trim().to_string()).filter(|s| !s.is_empty());
        trip_info.insert(r[tid].trim().to_string(), (r[tsid].trim().to_string(), b));
    }
    // stop_times grouped by trip
    let (sh, srows) = g.table("stop_times.txt")?.context("stop_times.txt")?;
    let (s_tid, s_arr, s_dep, s_sid, s_seq) = (col(&sh, "trip_id").unwrap(), col(&sh, "arrival_time").unwrap(), col(&sh, "departure_time").unwrap(), col(&sh, "stop_id").unwrap(), col(&sh, "stop_sequence").unwrap());
    let (s_pu, s_do) = (col(&sh, "pickup_type"), col(&sh, "drop_off_type"));
    let mut by_trip: HashMap<String, Vec<(u32, Option<i64>, Option<i64>, u32, bool, bool)>> = HashMap::new();
    let mut unknown_stops = 0u64;
    for r in &srows {
        let sid = r[s_sid].trim();
        let Some(&l) = loc_of.get(sid) else {
            unknown_stops += 1;
            continue;
        };
        let flag = |c: Option<usize>| c.and_then(|c| r.get(c)).map(|x| x.trim() != "1").unwrap_or(true);
        by_trip.entry(r[s_tid].trim().to_string()).or_default().push((r[s_seq].trim().parse()?, hms(&r[s_arr]), hms(&r[s_dep]), l, flag(s_pu), flag(s_do)));
    }
    let base_min = |d: &NaiveDate| -> Result<i64> {
        let noon = tz.from_local_datetime(&d.and_time(NaiveTime::from_hms_opt(12, 0, 0).unwrap())).single().context("noon ambiguous")?;
        Ok(noon.timestamp() / 60 - 12 * 60)
    };
    let bases = [base_min(&days[0])?, base_min(&days[1])?];
    let lo = a.dep_start_unix_minutes;
    let hi = a.dep_start_unix_minutes + (a.dep_count - 1) * a.dep_step + a.max_travel as i64;
    let mut conns: Vec<Conn> = Vec::new();
    let mut n_trip_inst = 0u32;
    let mut inst_names: Vec<(String, u32)> = Vec::new();
    let mut skipped_missing_times = 0u64;
    let mut blocks: HashMap<(String, usize), Vec<(i64, u32)>> = HashMap::new();
    let mut trip_ids: Vec<String> = by_trip.keys().cloned().collect();
    trip_ids.sort();
    for tidk in trip_ids {
        let st = by_trip.get_mut(&tidk).unwrap();
        st.sort_by_key(|x| x.0);
        let Some((svc, blk)) = trip_info.get(&tidk) else { continue };
        let Some(act) = active.get(svc) else { continue };
        if st.iter().any(|x| x.1.is_none() || x.2.is_none()) {
            skipped_missing_times += 1;
            continue;
        }
        for k in 0..2 {
            if !act[k] {
                continue;
            }
            let b = bases[k];
            let first = b + st[0].2.unwrap();
            let last = b + st[st.len() - 1].1.unwrap();
            if last < lo || first > hi {
                continue;
            }
            let inst = n_trip_inst;
            n_trip_inst += 1;
            inst_names.push((format!("{}@{}", tidk, days[k]), inst));
            if let Some(bk) = blk {
                blocks.entry((bk.clone(), k)).or_default().push((first, inst));
            }
            for i in 0..st.len() - 1 {
                conns.push(Conn {
                    from: st[i].3,
                    to: st[i + 1].3,
                    dep: b + st[i].2.unwrap(),
                    arr: b + st[i + 1].1.unwrap(),
                    trip: inst,
                    pickup: st[i].4,
                    dropoff: st[i + 1].5,
                    last: i + 2 == st.len(),
                });
            }
        }
    }
    // trip instance -> GTFS trip id index, and the forbidden-transfer set
    let mut trip_idx: HashMap<String, u32> = HashMap::new();
    let mut inst_gtfs = vec![u32::MAX; n_trip_inst as usize];
    for (name, inst) in &inst_names {
        let tid = name.rsplit_once('@').map(|x| x.0).unwrap_or(name).to_string();
        let n = trip_idx.len() as u32;
        inst_gtfs[*inst as usize] = *trip_idx.entry(tid).or_insert(n);
    }
    let forb: std::collections::HashSet<(u32, u32, u32, u32)> = forb_raw
        .iter()
        .filter_map(|(a_, b_, x, y)| Some((*trip_idx.get(a_)?, *trip_idx.get(b_)?, *x, *y)))
        .collect();
    let mut succ: Vec<u32> = vec![u32::MAX; n_trip_inst as usize];
    for v in blocks.values_mut() {
        v.sort();
        for w in v.windows(2) {
            succ[w[0].1 as usize] = w[1].1;
        }
    }
    conns.sort_by_key(|c| (c.dep, c.arr));
    let prep_s = t0.elapsed().as_secs_f64();
    if let Some(p) = &a.active_stops_out {
        let mut cnt = vec![0u64; n_loc];
        for c in &conns {
            if c.pickup && c.dep >= lo && c.dep <= hi {
                cnt[c.from as usize] += 1;
            }
        }
        let mut w = std::io::BufWriter::new(std::fs::File::create(p)?);
        use std::io::Write as _;
        writeln!(w, "stop,departures")?;
        let mut n_act = 0;
        for (s, &k) in cnt.iter().enumerate() {
            if k > 0 {
                writeln!(w, "{},{}", s, k)?;
                n_act += 1;
            }
        }
        w.flush()?;
        eprintln!("active stops: {} of {} locations have a boardable departure in [{}, {}]", n_act, n_loc, crate::motis_batch::iso(lo), crate::motis_batch::iso(hi));
    }
    let trip_name: Vec<String> = {
        // trip instance -> GTFS trip_id (for explanations)
        let mut v = vec![String::new(); n_trip_inst as usize];
        for (name, inst) in &inst_names {
            v[*inst as usize] = name.clone();
        }
        v
    };
    let explain: Vec<(u32, usize, u32)> = a
        .explain
        .iter()
        .map(|x| {
            let p: Vec<&str> = x.split(':').collect();
            Ok((p[0].parse()?, p[1].parse()?, p[2].parse()?))
        })
        .collect::<Result<_>>()?;
    let mut explanations = Vec::new();

    let access = read_connectors(&a.access)?;
    let mut acc: Vec<Vec<(u32, i64)>> = vec![vec![]; a.n_origins as usize];
    for (p, s, m) in access {
        acc[p as usize].push((s, m as i64));
    }
    let to_stops = a.target == "stops";
    let n_dests = if to_stops { n_loc as u32 } else { a.n_dests.context("--n-dests")? };
    let mut egr: Vec<Vec<(u32, i64)>> = vec![vec![]; n_loc];
    let mut dir: Vec<Vec<(u32, i64)>> = vec![vec![]; a.n_origins as usize];
    if !to_stops {
        for (d, s, m) in read_connectors(a.egress.as_ref().context("--egress")?)? {
            egr[s as usize].push((d, m as i64));
        }
        if let Some(p) = &a.direct {
            for (o, d, m) in read_connectors(p)? {
                dir[o as usize].push((d, m as i64));
            }
        }
    }
    // stock: transit arrivals only if strictly earlier than the access walk (Nigiri
    // one_to_all + stock egress from rounds >= 1); strict: every transit arrival;
    // walkstop: stock rounds plus round 0 (walk origin->stop->destination allowed),
    // which equals min(transit, access walk) per stop.
    let (stock, walkstop) = match a.semantics.as_str() {
        "stock" => (true, false),
        "strict" => (false, false),
        "walkstop" => (false, true),
        s => bail!("unknown semantics {s}"),
    };
    let deps: Vec<i64> = (0..a.dep_count).map(|i| a.dep_start_unix_minutes + i * a.dep_step).collect();
    let header = Header { n_origins_total: a.n_origins, n_dests, n_departures: deps.len() as u32, origin_from: 0, origin_to: a.n_origins, max_travel: a.max_travel, flags: 0, target: if to_stops { 1 } else { 0 }, departures: deps.clone() };
    let mut w = Writer::create(&a.out, header)?;
    const INF: i64 = i64::MAX / 4;
    let cap = a.max_travel as i64;
    let mut ready = vec![INF; n_loc];
    let mut arr_t = vec![INF; n_loc];
    let mut arr0 = vec![INF; n_loc];
    let mut boarded = vec![false; n_trip_inst as usize];
    // witness bookkeeping: how each stop's transit arrival / readiness was set
    let mut arr_src = vec![(u32::MAX, u32::MAX); n_loc]; // (connection idx, via footpath from stop)
    let mut ready_src = vec![u32::MAX; n_loc]; // connection idx (u32::MAX = access walk)
    let mut board = vec![(u32::MAX, 0i64); n_trip_inst as usize]; // (boarding stop, time)
    // Exact eligibility-aware readiness (only when forbidden trip-to-trip rules exist).
    // A readiness label at boarding stop Y comes from (source trip A, alighting stop X) or
    // from the access walk. Its eligibility for an onward trip B depends only on whether
    // (A, B, X, Y) is forbidden. Labels whose (A, X, Y) appears in no rule are eligible for
    // every B ("universal"): only the earliest universal label per stop matters. Restricted
    // labels are kept per distinct (trip instance, X) without any cap, and dropped only
    // when an equal-or-earlier universal label dominates them. This is exact (no bounded
    // label set; earlier versions kept <= 6 labels per stop and merged labels by source
    // trip across different alighting stops, which could miss a legal boarding).
    let use_labels = !forb.is_empty();
    let restricted: std::collections::HashSet<(u32, u32, u32)> = forb.iter().map(|&(a_, _, x, y)| (a_, x, y)).collect();
    let mut uni = vec![INF; if use_labels { n_loc } else { 0 }];
    let mut res: Vec<Vec<(i64, u32, u32)>> = vec![Vec::new(); if use_labels { n_loc } else { 0 }];
    let mut touched: Vec<u32> = Vec::new();
    let mut max_restricted = 0usize;
    let mut restricted_inserts = 0u64;
    let mut forbidden_hits = 0u64;
    let add_label = |uni: &mut Vec<i64>, res: &mut Vec<Vec<(i64, u32, u32)>>, touched: &mut Vec<u32>, max_r: &mut usize, ins: &mut u64, y: usize, t: i64, src: u32, x: u32| {
        if uni[y] == INF && res[y].is_empty() {
            touched.push(y as u32);
        }
        let is_restricted = src != u32::MAX && restricted.contains(&(inst_gtfs[src as usize], x, y as u32));
        if !is_restricted {
            if t < uni[y] {
                uni[y] = t;
            }
            return;
        }
        if t >= uni[y] {
            return; // dominated by an equal-or-earlier universal label
        }
        *ins += 1;
        let v = &mut res[y];
        if let Some(e) = v.iter_mut().find(|e| e.1 == src && e.2 == x) {
            if t < e.0 {
                e.0 = t;
            }
        } else {
            v.push((t, src, x));
            *max_r = (*max_r).max(v.len());
        }
    };
    let t1 = std::time::Instant::now();
    let mut scanned = 0u64;
    for &o in &a.sample {
        let mut rec = Record { origin: o, tt: vec![UNREACHABLE; deps.len() * n_dests as usize], transfers: vec![], via: vec![] };
        for (di, &t) in deps.iter().enumerate() {
            ready.iter_mut().for_each(|x| *x = INF);
            arr_t.iter_mut().for_each(|x| *x = INF);
            arr0.iter_mut().for_each(|x| *x = INF);
            boarded.iter_mut().for_each(|x| *x = false);
            for &st in &touched {
                uni[st as usize] = INF;
                res[st as usize].clear();
            }
            touched.clear();
            let want = explain.iter().any(|e| e.0 == o && e.1 == di);
            for &(s, m) in &acc[o as usize] {
                ready[s as usize] = ready[s as usize].min(t + m);
                arr0[s as usize] = arr0[s as usize].min(t + m);
                if use_labels {
                    add_label(&mut uni, &mut res, &mut touched, &mut max_restricted, &mut restricted_inserts, s as usize, t + m, u32::MAX, u32::MAX);
                }
            }
            let start = conns.partition_point(|c| c.dep < t);
            for (ci, c) in conns[start..].iter().enumerate() {
                let ci = (start + ci) as u32;
                if c.dep > t + cap {
                    break;
                }
                scanned += 1;
                let ti = c.trip as usize;
                if !boarded[ti] {
                    let ok = c.pickup
                        && ready[c.from as usize] <= c.dep
                        && (!use_labels
                            || uni[c.from as usize] <= c.dep
                            || res[c.from as usize].iter().any(|&(lt, src, x)| {
                                lt <= c.dep && lt < uni[c.from as usize] && !forb.contains(&(inst_gtfs[src as usize], inst_gtfs[ti], x, c.from))
                            }));
                    if use_labels && c.pickup && ready[c.from as usize] <= c.dep && !ok {
                        forbidden_hits += 1;
                    }
                    if ok {
                        boarded[ti] = true;
                        if want {
                            board[ti] = (c.from, c.dep);
                        }
                    } else {
                        continue;
                    }
                }
                if c.last && succ[ti] != u32::MAX {
                    boarded[succ[ti] as usize] = true;
                }
                if !c.dropoff || c.arr - t > cap {
                    continue;
                }
                let to = c.to as usize;
                if c.arr < arr_t[to] {
                    arr_t[to] = c.arr;
                    if want {
                        arr_src[to] = (ci, u32::MAX);
                    }
                }
                let r = c.arr + transfer_time[to];
                if use_labels {
                    add_label(&mut uni, &mut res, &mut touched, &mut max_restricted, &mut restricted_inserts, to, r, c.trip, c.to);
                }
                if r < ready[to] {
                    ready[to] = r;
                    if want {
                        ready_src[to] = ci;
                    }
                }
                for &(f, d) in &fps[to] {
                    let x = c.arr + d;
                    if use_labels && x - t <= cap {
                        add_label(&mut uni, &mut res, &mut touched, &mut max_restricted, &mut restricted_inserts, f as usize, x, c.trip, c.to);
                    }
                    if x - t <= cap {
                        if x < arr_t[f as usize] {
                            arr_t[f as usize] = x;
                            if want {
                                arr_src[f as usize] = (ci, c.to);
                            }
                        }
                        if x < ready[f as usize] {
                            ready[f as usize] = x;
                            if want {
                                ready_src[f as usize] = ci;
                            }
                        }
                    }
                }
            }
            if want && !to_stops {
                for &(eo, edi, ed) in explain.iter().filter(|e| e.0 == o && e.1 == di) {
                    // best egress stop under each semantics
                    let mut best: Vec<serde_json::Value> = Vec::new();
                    for s in 0..n_loc {
                        for &(d, e) in &egr[s] {
                            if d != ed || arr_t[s] >= INF || arr_t[s] - t + e > cap {
                                continue;
                            }
                            let total = arr_t[s] - t + e;
                            let (cidx, via) = arr_src[s];
                            let c = &conns[cidx as usize];
                            // walk back the vehicle legs
                            let mut legs = Vec::new();
                            let mut cur = cidx;
                            for _ in 0..16 {
                                let cc = &conns[cur as usize];
                                let (bs, bt) = board[cc.trip as usize];
                                legs.push(serde_json::json!({"trip": trip_name[cc.trip as usize], "board_stop": bs, "board_time": crate::motis_batch::iso(bt), "alight_stop": cc.to, "alight_time": crate::motis_batch::iso(cc.arr)}));
                                if bs == u32::MAX || ready_src[bs as usize] == u32::MAX {
                                    let acc_m = acc[o as usize].iter().find(|x| x.0 == bs).map(|x| x.1);
                                    legs.push(serde_json::json!({"access_walk_to": bs, "access_minutes": acc_m}));
                                    break;
                                }
                                cur = ready_src[bs as usize];
                            }
                            legs.reverse();
                            best.push(serde_json::json!({
                                "egress_stop": s, "transit_arrival_min": arr_t[s] - t, "access_walk_arrival_min": if arr0[s] < INF { Some(arr0[s] - t) } else { None },
                                "kept_by_stock": arr_t[s] < arr0[s], "egress_min": e, "total": total,
                                "last_footpath_from": if via == u32::MAX { None } else { Some(via) }, "last_alight": c.to, "legs": legs,
                            }));
                        }
                    }
                    best.sort_by_key(|v| v["total"].as_i64().unwrap_or(i64::MAX));
                    best.truncate(3);
                    let direct = dir[eo as usize].iter().find(|x| x.0 == ed).map(|x| x.1);
                    explanations.push(serde_json::json!({"origin": eo, "departure_index": edi, "departure": crate::motis_batch::iso(t), "dest": ed, "direct_walk_min": direct, "best_transit_options": best}));
                }
            }
            let row = &mut rec.tt[di * n_dests as usize..(di + 1) * n_dests as usize];
            if to_stops {
                for s in 0..n_loc {
                    let v = arr0[s].min(arr_t[s]);
                    if v - t <= cap {
                        row[s] = (v - t) as u16;
                    }
                }
            } else {
                for &(d, m) in &dir[o as usize] {
                    if m <= cap && (m as u16) < row[d as usize] {
                        row[d as usize] = m as u16;
                    }
                }
                for s in 0..n_loc {
                    let mut v = arr_t[s];
                    if walkstop {
                        v = v.min(arr0[s]);
                    }
                    if v >= INF || (stock && v >= arr0[s]) {
                        continue;
                    }
                    v -= t;
                    for &(d, e) in &egr[s] {
                        let tot = v + e;
                        if tot <= cap && (tot as u16) < row[d as usize] {
                            row[d as usize] = tot as u16;
                        }
                    }
                }
            }
        }
        w.write(&rec)?;
    }
    w.finish()?;
    let _ = DIRECT_WALK;
    println!(
        "{}",
        serde_json::to_string_pretty(&serde_json::json!({
            "tool": "vr-access oracle",
            "timezone": tzs,
            "service_days": [days[0].to_string(), days[1].to_string()],
            "trip_instances": n_trip_inst,
            "connections": conns.len(),
            "trips_skipped_missing_times": skipped_missing_times,
            "stop_times_unknown_stop": unknown_stops,
            "warnings": warnings,
            "semantics": a.semantics,
            "target": a.target,
            "origins": a.sample.len(),
            "departures": deps.len(),
            "connections_scanned": scanned,
            "prepare_seconds": prep_s,
            "scan_seconds": t1.elapsed().as_secs_f64(),
            "explanations": explanations,
            "forbidden_transfer_rules": forb.len(),
            "boardings_blocked_by_rules": forbidden_hits,
            "readiness_mode": if use_labels { "exact-eligibility-aware" } else { "single-label (no forbidden rules)" },
            "restricted_label_inserts": restricted_inserts,
            "max_restricted_labels_per_stop": max_restricted,
            "footpaths_skipped_virtual": skipped_virtual_fp,
        }))?
    );
    Ok(())
}
