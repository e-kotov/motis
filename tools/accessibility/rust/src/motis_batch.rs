//! Stock MOTIS endpoint control: generate `motis batch` GET queries for
//! /api/experimental/one-to-many-intermodal and convert its responses into a VRTT stream.

use crate::geo::{LatLon, Table};
use crate::stream::{Header, Record, Writer, DIRECT_WALK, FLAG_TRANSFERS, TR_UNREACHABLE, UNREACHABLE};
use anyhow::{bail, Context, Result};
use chrono::{TimeZone, Utc};
use std::collections::BTreeMap;
use std::io::{BufRead, Write};
use std::path::Path;

pub struct QueryParams {
    pub max_travel: u16,
    pub pre_post_direct_seconds: u32,
    pub pedestrian_speed: f64,
    pub max_matching_distance: f64,
    pub chunk: usize,
}

fn coord(p: &LatLon) -> String {
    format!("{:.7};{:.7}", p.lat, p.lon)
}

pub fn iso(unix_minutes: i64) -> String {
    Utc.timestamp_opt(unix_minutes * 60, 0).unwrap().format("%Y-%m-%dT%H:%M:%SZ").to_string()
}

/// Writes queries.txt (one GET target per line) and map.csv
/// (line,origin,departure_index,dest_begin,dest_end).
pub fn write_queries(origins: &[LatLon], sample: &[u32], dests: &[LatLon], departures: &[i64], q: &QueryParams, out: &Path, map: &Path) -> Result<usize> {
    let mut w = std::io::BufWriter::new(std::fs::File::create(out)?);
    let mut m = std::io::BufWriter::new(std::fs::File::create(map)?);
    writeln!(m, "line,origin,departure_index,dest_begin,dest_end")?;
    let mut line = 0usize;
    for &o in sample {
        for (di, &dep) in departures.iter().enumerate() {
            let mut b = 0;
            while b < dests.len() {
                let e = (b + q.chunk).min(dests.len());
                let many: Vec<String> = dests[b..e].iter().map(coord).collect();
                writeln!(
                    w,
                    "/api/experimental/one-to-many-intermodal?one={}&many={}&time={}&maxTravelTime={}&maxPreTransitTime={}&maxPostTransitTime={}&maxDirectTime={}&pedestrianSpeed={}&maxMatchingDistance={}",
                    coord(&origins[o as usize]),
                    many.join(","),
                    iso(dep),
                    q.max_travel,
                    q.pre_post_direct_seconds,
                    q.pre_post_direct_seconds,
                    q.pre_post_direct_seconds,
                    q.pedestrian_speed,
                    q.max_matching_distance
                )?;
                writeln!(m, "{},{},{},{},{}", line, o, di, b, e)?;
                line += 1;
                b = e;
            }
        }
    }
    w.flush()?;
    m.flush()?;
    Ok(line)
}

/// Converts responses (one JSON per line, same order as queries) to a VRTT stream.
/// Transit: min over the Pareto set (seconds are whole minutes), ties -> fewer transfers.
/// Direct street walk: ceil(seconds / 60). Final: walk wins ties; > cap -> unreachable.
pub fn convert_responses(map: &Path, responses: &Path, n_origins_total: u32, n_dests: u32, departures: &[i64], cap: u16, out: &Path) -> Result<serde_json::Value> {
    let t = Table::read(map)?;
    let mut lines = std::io::BufReader::new(std::fs::File::open(responses)?).lines();
    let mut recs: BTreeMap<u32, Record> = BTreeMap::new();
    let n_dep = departures.len();
    let nd = n_dests as usize;
    let (empty, mut non_minute, mut stock_over_cap) = (0u64, 0u64, 0u64);
    for row in &t.rows {
        let line: usize = row[0].parse()?;
        let o: u32 = row[1].parse()?;
        let di: usize = row[2].parse()?;
        let (b, e): (usize, usize) = (row[3].parse()?, row[4].parse()?);
        let text = lines.next().with_context(|| format!("missing response line {}", line))??;
        if text.trim().is_empty() {
            bail!("empty response for query line {} (stock batch error)", line);
        }
        let v: serde_json::Value = serde_json::from_str(&text).with_context(|| format!("parse response {}", line))?;
        let street = v["street_durations"].as_array().context("street_durations")?;
        let transit = v["transit_durations"].as_array().context("transit_durations")?;
        if street.len() != e - b || transit.len() != e - b {
            bail!("response {} has {} / {} entries, expected {}", line, street.len(), transit.len(), e - b);
        }
        let rec = recs.entry(o).or_insert_with(|| Record { origin: o, tt: vec![UNREACHABLE; n_dep * nd], transfers: vec![TR_UNREACHABLE; n_dep * nd], via: vec![] });
        for k in 0..(e - b) {
            let mut best: Option<(u32, u8)> = None;
            for entry in transit[k].as_array().context("pareto set")? {
                let secs = entry["duration"].as_f64().context("duration")?;
                let tr = entry["transfers"].as_u64().context("transfers")? as u8;
                if (secs / 60.0).fract().abs() > 1e-6 {
                    non_minute += 1;
                }
                let m = (secs / 60.0).round() as u32;
                if best.map_or(true, |(bm, bt)| m < bm || (m == bm && tr < bt)) {
                    best = Some((m, tr));
                }
            }
            let direct = street[k]["duration"].as_f64().map(|s| ((s / 60.0) - 1e-9).ceil().max(0.0) as u32);
            let idx = di * nd + b + k;
            let mut tt = UNREACHABLE;
            let mut trn = TR_UNREACHABLE;
            if let Some((m, tr)) = best {
                if m <= cap as u32 {
                    tt = m as u16;
                    trn = tr;
                } else {
                    stock_over_cap += 1;
                }
            }
            if let Some(dm) = direct {
                if dm <= cap as u32 && (tt == UNREACHABLE || dm <= tt as u32) {
                    tt = dm as u16;
                    trn = DIRECT_WALK;
                }
            }
            rec.tt[idx] = tt;
            rec.transfers[idx] = trn;
        }
    }
    if lines.next().is_some() {
        bail!("more responses than map rows");
    }
    let header = Header {
        n_origins_total,
        n_dests,
        n_departures: n_dep as u32,
        origin_from: 0,
        origin_to: n_origins_total,
        max_travel: cap,
        flags: FLAG_TRANSFERS,
        target: 0,
        departures: departures.to_vec(),
    };
    let mut w = Writer::create(out, header)?;
    for r in recs.values() {
        w.write(r)?;
    }
    w.finish()?;
    Ok(serde_json::json!({
        "responses": t.rows.len(),
        "origins": recs.len(),
        "empty_responses": empty,
        "transit_durations_not_whole_minutes": non_minute,
        "stock_transit_over_cap_dropped": stock_over_cap,
    }))
}
