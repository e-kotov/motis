//! Station-to-station workloads (stream target = stops): per-origin summaries and the
//! stock `/api/v6/one-to-all` control (queries + response conversion).

use crate::geo::Table;
use crate::motis_batch::iso;
use crate::stream::{Header, Reader, Record, Writer, FLAG_TRANSFERS, TR_UNREACHABLE, UNREACHABLE};
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io::{BufRead, Write};
use std::path::Path;

/// `stop` column of a CSV (active-stop list or stop-origins points).
pub fn read_stop_list(p: &Path) -> Result<Vec<u32>> {
    let t = Table::read(p)?;
    let c = t.col("stop")?;
    t.rows.iter().map(|r| Ok(r[c].parse()?)).collect()
}

/// Per origin and departure: reachable active stops (<= cap), their travel-time sum,
/// all reachable locations, and a SHA-256 digest of the full arrival vector (u16 LE),
/// so two engines can be compared without storing matrices.
pub fn stop_summary(stream: &Path, active: &[u32], out: &Path) -> Result<serde_json::Value> {
    let mut rd = Reader::open(stream)?;
    let h = rd.header.clone();
    if h.target != 1 {
        bail!("stop-summary needs a stops stream (--target stops)");
    }
    let n = h.n_dests as usize;
    let mut is_active = vec![false; n];
    for &s in active {
        is_active[s as usize] = true;
    }
    let mut w = std::io::BufWriter::new(std::fs::File::create(out)?);
    writeln!(w, "origin,departure_index,reachable_active,reachable_all,sum_tt_active,digest")?;
    let mut rec = Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
    let mut rows = Vec::new();
    let mut total = Sha256::new();
    let (mut n_rec, mut sum_reach) = (0u64, 0u64);
    while rd.next_into(&mut rec)? {
        n_rec += 1;
        for di in 0..h.n_departures as usize {
            let row = &rec.tt[di * n..(di + 1) * n];
            let (mut ra, mut rall, mut st) = (0u64, 0u64, 0u64);
            let mut hs = Sha256::new();
            for (s, &t) in row.iter().enumerate() {
                hs.update(t.to_le_bytes());
                if t != UNREACHABLE {
                    rall += 1;
                    if is_active[s] {
                        ra += 1;
                        st += t as u64;
                    }
                }
            }
            sum_reach += ra;
            rows.push((rec.origin, di, ra, rall, st, format!("{:x}", hs.finalize())));
        }
    }
    rows.sort();
    for r in &rows {
        writeln!(w, "{},{},{},{},{},{}", r.0, r.1, r.2, r.3, r.4, r.5)?;
        total.update(r.5.as_bytes());
    }
    w.flush()?;
    Ok(serde_json::json!({
        "records": n_rec, "active_stops": active.len(), "n_locations": n,
        "reachable_active_total": sum_reach, "digest_of_digests": format!("{:x}", total.finalize()),
    }))
}

fn urlenc(s: &str) -> String {
    s.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (b as char).to_string(),
            _ => format!("%{:02X}", b),
        })
        .collect()
}

/// queries.txt for stock one-to-all from stops; map.csv: line,origin,departure_index.
pub fn one_to_all_queries(stops: &Path, origins: &[u32], tag: &str, departures: &[i64], max_travel: u16, pre_seconds: u32, out: &Path, map: &Path) -> Result<usize> {
    let t = Table::read(stops)?;
    let id = t.col("id")?;
    let ids: Vec<String> = t.rows.iter().map(|r| r[id].to_string()).collect();
    let mut w = std::io::BufWriter::new(std::fs::File::create(out)?);
    let mut m = std::io::BufWriter::new(std::fs::File::create(map)?);
    writeln!(m, "line,origin,departure_index")?;
    let mut line = 0;
    for (oi, &s) in origins.iter().enumerate() {
        for (di, &dep) in departures.iter().enumerate() {
            writeln!(w, "/api/v6/one-to-all?one={}&time={}&maxTravelTime={}&maxPreTransitTime={}", urlenc(&format!("{}_{}", tag, ids[s as usize])), iso(dep), max_travel, pre_seconds)?;
            writeln!(m, "{},{},{}", line, oi, di)?;
            line += 1;
        }
    }
    w.flush()?;
    m.flush()?;
    Ok(line)
}

/// Converts one-to-all responses to a stops stream: duration (minutes) and k (number of
/// vehicles used; 0 = start/walk only), the same transfers-block convention as
/// `motis-accessibility route --target stops`.
pub fn from_one_to_all(map: &Path, responses: &Path, stops: &Path, tag: &str, n_origins: u32, departures: &[i64], cap: u16, out: &Path) -> Result<serde_json::Value> {
    let st = Table::read(stops)?;
    let id = st.col("id")?;
    let idx: HashMap<String, usize> = st.rows.iter().enumerate().map(|(i, r)| (format!("{}_{}", tag, &r[id]), i)).collect();
    let n = st.rows.len();
    let t = Table::read(map)?;
    let mut lines = std::io::BufReader::new(std::fs::File::open(responses)?).lines();
    let n_dep = departures.len();
    let mut recs: HashMap<u32, Record> = HashMap::new();
    let (mut unknown, mut over) = (0u64, 0u64);
    for row in &t.rows {
        let o: u32 = row[1].parse()?;
        let di: usize = row[2].parse()?;
        let text = lines.next().context("missing response")??;
        if text.trim().is_empty() {
            bail!("empty one-to-all response for map line {}", &row[0]);
        }
        let v: serde_json::Value = serde_json::from_str(&text)?;
        let rec = recs.entry(o).or_insert_with(|| Record { origin: o, tt: vec![UNREACHABLE; n_dep * n], transfers: vec![TR_UNREACHABLE; n_dep * n], via: vec![] });
        for e in v["all"].as_array().context("all")? {
            let sid = e["place"]["stopId"].as_str().unwrap_or("");
            let Some(&s) = idx.get(sid) else {
                unknown += 1;
                continue;
            };
            let d = e["duration"].as_i64().context("duration")?;
            let k = e["k"].as_i64().unwrap_or(0);
            if d < 0 || d > cap as i64 {
                over += 1;
                continue;
            }
            let i = di * n + s;
            if (d as u16) < rec.tt[i] {
                rec.tt[i] = d as u16;
                rec.transfers[i] = k as u8;
            }
        }
    }
    let header = Header { n_origins_total: n_origins, n_dests: n as u32, n_departures: n_dep as u32, origin_from: 0, origin_to: n_origins, max_travel: cap, flags: FLAG_TRANSFERS, target: 1, departures: departures.to_vec() };
    let mut w = Writer::create(out, header)?;
    let mut keys: Vec<_> = recs.keys().copied().collect();
    keys.sort();
    for k in keys {
        w.write(&recs[&k])?;
    }
    w.finish()?;
    Ok(serde_json::json!({"origins": recs.len(), "unknown_stop_ids": unknown, "over_cap_dropped": over}))
}
