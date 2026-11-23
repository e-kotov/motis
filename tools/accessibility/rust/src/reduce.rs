//! Population-weighted cumulative accessibility per origin from a VRTT stream.
//!
//! Semantics (all thresholds inclusive: a destination with travel time T counts at T):
//! - `fixed`     : one departure (index `fixed_index`), TT = arrival - departure
//!                 (initial wait at the origin counted).
//! - `best`      : per destination the minimum TT over all departures in the stream
//!                 (with a 1-minute departure grid this equals the minute-resolution
//!                 profile minimum over the window).
//! - `meancount` : mean over departures of the reachable population at T
//!                 (accessibility averaged over departure minutes; non-integer).
//! - `meantt`    : per destination the mean TT over departures, only if it is reachable
//!                 within the cap at every departure (otherwise excluded); population
//!                 with mean TT <= T.
//! - `meanttcap` : as `meantt`, but an unreachable departure counts as `cap` minutes;
//!                 destinations unreachable at every departure are excluded.
//! `c_T` is reported for integer T = 1..=cap. `t_N` = travel time at which the
//! cumulative population first reaches N (for meancount: mean over departures of the
//! per-departure t_N over the departures where N is reached; `ndef_N` = departures
//! where it is not reached within the cap).
//!
//! Options: `exclude_self` drops destination index == origin index (origins and
//! destinations are the same cell list). With `continuous`, cells whose best option
//! is the direct walk (transfer code 254) use the continuous straight-line walking
//! minutes (distance / speed) instead of the ceiled stream minutes; this changes only
//! t_N and meantt, never c_T at integer T (ceil(x) <= T  <=>  x <= T).

use crate::geo::{haversine_m, Grid, LatLon};
use crate::stream::{Reader, Record, DIRECT_WALK, UNREACHABLE};
use anyhow::{bail, Result};
use std::io::Write;
use std::path::Path;

pub struct Continuous<'a> {
    pub origins: &'a [LatLon],
    pub dests: &'a [LatLon],
    pub grid: Grid,
    pub speed_mps: f64,
    pub max_m: f64,
}

pub struct Params<'a> {
    pub weights: &'a [f64],
    pub fixed_index: usize,
    pub cumulative: &'a [f64],
    pub threads: usize,
    /// per origin: destination index of the same cell (excluded), if any
    pub self_index: Option<Vec<Option<usize>>>,
    pub continuous: Option<Continuous<'a>>,
}

pub struct OriginResult {
    pub origin: u32,
    /// [semantics][T] cumulative population for T = 0..=cap
    pub cum: [Vec<f64>; 5],
    /// [semantics][N] time to reach N (None = not reached)
    pub t_n: [Vec<Option<f64>>; 5],
    pub meancount_ndef: Vec<u32>,
    pub reachable_pairs_fixed: u64,
}

pub const SEMANTICS: [&str; 5] = ["fixed", "best", "meancount", "meantt", "meanttcap"];

/// First time the cumulative population reaches n. `c` holds integer-bin cumulative
/// counts (an item with value v is in bin ceil(v)); `frac` lists items with
/// non-integer values (value, weight) so the crossing inside a bin is exact.
fn t_reach(c: &[f64], n: f64, frac: &[(f64, f64)]) -> Option<f64> {
    let ts = c.iter().position(|&x| x >= n)?;
    let base = if ts == 0 { 0.0 } else { c[ts - 1] };
    let mut items: Vec<(f64, f64)> = frac.iter().copied().filter(|&(v, _)| v > ts as f64 - 1.0 && v < ts as f64).collect();
    items.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    let mut acc = base;
    for (v, w) in items {
        acc += w;
        if acc >= n {
            return Some(v);
        }
    }
    Some(ts as f64)
}

fn cumulate(hist: &[f64], out: &mut [f64]) {
    let mut acc = 0.0;
    for (t, h) in hist.iter().enumerate() {
        acc += h;
        out[t] = acc;
    }
}

pub fn reduce_record(rec: &Record, n_dests: usize, n_dep: usize, cap: usize, p: &Params) -> Result<OriginResult> {
    let w = p.weights;
    let o = rec.origin as usize;
    let skip = p.self_index.as_ref().and_then(|v| v.get(o).copied().flatten()).unwrap_or(usize::MAX);
    // continuous direct-walk minutes for this origin (dest, minutes)
    let mut cont: Vec<(usize, f64)> = Vec::new();
    if let Some(c) = &p.continuous {
        if rec.transfers.is_empty() {
            bail!("continuous mode needs a stream with the transfers block");
        }
        let op = c.origins[o];
        c.grid.candidates(&op, c.max_m, |j| {
            let d = haversine_m(op, c.dests[j as usize]);
            if d <= c.max_m && j as usize != skip {
                cont.push((j as usize, d / c.speed_mps / 60.0));
            }
        });
        cont.sort_by_key(|x| x.0);
    }
    let cont_of = |d: usize| cont.binary_search_by_key(&d, |x| x.0).ok().map(|i| cont[i].1);
    let value = |row_tt: &[u16], row_tr: &[u8], d: usize| -> f64 {
        let t = row_tt[d];
        if !cont.is_empty() && row_tr[d] == DIRECT_WALK {
            if let Some(v) = cont_of(d) {
                return v;
            }
        }
        t as f64
    };
    let mut hist = vec![0f64; cap + 1];
    let mut cum = [vec![0f64; cap + 1], vec![0f64; cap + 1], vec![0f64; cap + 1], vec![0f64; cap + 1], vec![0f64; cap + 1]];
    let mut best = vec![f64::INFINITY; n_dests];
    let mut sum = vec![0f64; n_dests];
    let mut nreach = vec![0u32; n_dests];
    let mut tn_mean = vec![0f64; p.cumulative.len()];
    let mut tn_cnt = vec![0u32; p.cumulative.len()];
    let mut reachable_pairs_fixed = 0u64;
    let mut c = vec![0f64; cap + 1];
    let mut t_fixed = vec![None; p.cumulative.len()];
    let empty: [u8; 0] = [];
    for di in 0..n_dep {
        let row = &rec.tt[di * n_dests..(di + 1) * n_dests];
        let row_tr: &[u8] = if rec.transfers.is_empty() { &empty } else { &rec.transfers[di * n_dests..(di + 1) * n_dests] };
        hist.iter_mut().for_each(|h| *h = 0.0);
        let mut nr = 0u64;
        for (d, &t) in row.iter().enumerate() {
            if t == UNREACHABLE || d == skip {
                continue;
            }
            if t as usize > cap {
                bail!("travel time {} above cap {}", t, cap);
            }
            nr += 1;
            hist[t as usize] += w[d];
            let v = if cont.is_empty() { t as f64 } else { value(row, row_tr, d) };
            if v < best[d] {
                best[d] = v;
            }
            sum[d] += v;
            nreach[d] += 1;
        }
        cumulate(&hist, &mut c);
        for t in 0..=cap {
            cum[2][t] += c[t];
        }
        let frac: Vec<(f64, f64)> = cont.iter().filter(|&&(d, _)| row[d] != UNREACHABLE && !row_tr.is_empty() && row_tr[d] == DIRECT_WALK).map(|&(d, v)| (v, w[d])).collect();
        for (k, &n) in p.cumulative.iter().enumerate() {
            let tn = t_reach(&c, n, &frac);
            if let Some(t) = tn {
                tn_mean[k] += t;
                tn_cnt[k] += 1;
            }
            if di == p.fixed_index {
                t_fixed[k] = tn;
            }
        }
        if di == p.fixed_index {
            cum[0].copy_from_slice(&c);
            reachable_pairs_fixed = nr;
        }
    }
    for v in cum[2].iter_mut() {
        *v /= n_dep as f64;
    }
    // best
    hist.iter_mut().for_each(|h| *h = 0.0);
    let mut best_frac = Vec::new();
    for (d, &v) in best.iter().enumerate() {
        if v.is_finite() {
            hist[v.ceil() as usize] += w[d];
            if v.fract() != 0.0 {
                best_frac.push((v, w[d]));
            }
        }
    }
    cumulate(&hist, &mut cum[1]);
    // meantt
    let mut mt: Vec<(f64, f64)> = (0..n_dests).filter(|&d| d != skip && nreach[d] as usize == n_dep).map(|d| (sum[d] / n_dep as f64, w[d])).collect();
    mt.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    let mut mc: Vec<(f64, f64)> = (0..n_dests)
        .filter(|&d| d != skip && nreach[d] > 0)
        .map(|d| ((sum[d] + (n_dep as u32 - nreach[d]) as f64 * cap as f64) / n_dep as f64, w[d]))
        .collect();
    mc.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    for (k, list) in [(3usize, &mt), (4usize, &mc)] {
        let mut j = 0;
        let mut acc = 0.0;
        for t in 0..=cap {
            while j < list.len() && list[j].0 <= t as f64 {
                acc += list[j].1;
                j += 1;
            }
            cum[k][t] = acc;
        }
    }
    let first_reach = |list: &[(f64, f64)], n: f64| -> Option<f64> {
        let mut acc = 0.0;
        for &(m, wt) in list {
            acc += wt;
            if acc >= n {
                return Some(m);
            }
        }
        None
    };
    let mut t_n: [Vec<Option<f64>>; 5] = Default::default();
    for (k, &n) in p.cumulative.iter().enumerate() {
        t_n[0].push(t_fixed[k]);
        t_n[1].push(t_reach(&cum[1], n, &best_frac));
        t_n[2].push(if tn_cnt[k] > 0 { Some(tn_mean[k] / tn_cnt[k] as f64) } else { None });
        t_n[3].push(first_reach(&mt, n));
        t_n[4].push(first_reach(&mc, n));
    }
    let meancount_ndef = tn_cnt.iter().map(|&c| n_dep as u32 - c).collect();
    Ok(OriginResult { origin: rec.origin, cum, t_n, meancount_ndef, reachable_pairs_fixed })
}

pub struct Summary {
    pub results: Vec<OriginResult>,
    pub cap: usize,
    pub n_dep: usize,
    pub n_dests: usize,
    pub records: u64,
}

pub fn reduce_streams(paths: &[std::path::PathBuf], p: &Params) -> Result<Summary> {
    let mut results: Vec<OriginResult> = Vec::new();
    let mut cfg: Option<(usize, usize, usize, Vec<i64>)> = None;
    let mut records = 0u64;
    for path in paths {
        let mut rd = Reader::open(path)?;
        let h = rd.header.clone();
        if h.target != 0 {
            bail!("{}: reduce needs a destination stream (target=dests)", path.display());
        }
        let this = (h.n_dests as usize, h.n_departures as usize, h.max_travel as usize, h.departures.clone());
        match &cfg {
            None => cfg = Some(this.clone()),
            Some(c) if *c != this => bail!("{}: header differs from first stream", path.display()),
            _ => {}
        }
        if p.weights.len() != h.n_dests as usize {
            bail!("weights ({}) != n_dests ({})", p.weights.len(), h.n_dests);
        }
        if p.fixed_index >= h.n_departures as usize {
            bail!("fixed index out of range");
        }
        if let Some(v) = &p.self_index {
            if v.len() != h.n_origins_total as usize {
                bail!("self map has {} origins, stream {}", v.len(), h.n_origins_total);
            }
        }
        let (n_dests, n_dep, cap) = (this.0, this.1, this.2);
        loop {
            let mut batch = Vec::new();
            for _ in 0..(p.threads * 8).max(1) {
                let mut rec = Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
                if !rd.next_into(&mut rec)? {
                    break;
                }
                batch.push(rec);
            }
            if batch.is_empty() {
                break;
            }
            records += batch.len() as u64;
            let chunk = batch.len().div_ceil(p.threads.max(1));
            let out: Vec<Result<Vec<OriginResult>>> = std::thread::scope(|s| {
                let hs: Vec<_> = batch
                    .chunks(chunk)
                    .map(|c| s.spawn(move || c.iter().map(|r| reduce_record(r, n_dests, n_dep, cap, p)).collect::<Result<Vec<_>>>()))
                    .collect();
                hs.into_iter().map(|h| h.join().unwrap()).collect()
            });
            for r in out {
                results.extend(r?);
            }
        }
    }
    let (n_dests, n_dep, cap, _) = cfg.ok_or_else(|| anyhow::anyhow!("no streams"))?;
    results.sort_by_key(|r| r.origin);
    for w in results.windows(2) {
        if w[0].origin == w[1].origin {
            bail!("origin {} appears twice", w[0].origin);
        }
    }
    Ok(Summary { results, cap, n_dep, n_dests, records })
}

fn fmt_opt(v: Option<f64>) -> String {
    v.map(|x| format!("{}", x)).unwrap_or_default()
}

pub fn write_csv(s: &Summary, cumulative: &[f64], out: &Path) -> Result<()> {
    let mut w = std::io::BufWriter::new(std::fs::File::create(out)?);
    write!(w, "origin,semantics")?;
    for t in 1..=s.cap {
        write!(w, ",c_{}", t)?;
    }
    for n in cumulative {
        write!(w, ",t_{}", *n as u64)?;
    }
    for n in cumulative {
        write!(w, ",ndef_{}", *n as u64)?;
    }
    writeln!(w)?;
    for r in &s.results {
        for (k, name) in SEMANTICS.iter().enumerate() {
            write!(w, "{},{}", r.origin, name)?;
            for t in 1..=s.cap {
                write!(w, ",{}", r.cum[k][t])?;
            }
            for v in &r.t_n[k] {
                write!(w, ",{}", fmt_opt(*v))?;
            }
            for i in 0..cumulative.len() {
                if k == 2 {
                    write!(w, ",{}", r.meancount_ndef[i])?;
                } else {
                    write!(w, ",")?;
                }
            }
            writeln!(w)?;
        }
    }
    w.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stream::DIRECT_WALK;

    fn rec(origin: u32, tt: Vec<u16>, tr: Vec<u8>) -> Record {
        Record { origin, tt, transfers: tr, via: vec![] }
    }

    #[test]
    fn inclusive_threshold_and_semantics() {
        // 3 destinations, 2 departures, cap 10.
        // dep0: d0=0 (self), d1=5, d2=unreachable; dep1: d0=0, d1=7, d2=10
        let r = rec(0, vec![0, 5, UNREACHABLE, 0, 7, 10], vec![]);
        let w = [100.0, 10.0, 1.0];
        let p = Params { weights: &w, fixed_index: 0, cumulative: &[10.0, 11.0], threads: 1, self_index: Some(vec![Some(0)]), continuous: None };
        let o = reduce_record(&r, 3, 2, 10, &p).unwrap();
        // fixed (dep0): 10 people reachable at T>=5, never 11
        assert_eq!(o.cum[0][4], 0.0);
        assert_eq!(o.cum[0][5], 10.0);
        assert_eq!(o.cum[0][10], 10.0);
        assert_eq!(o.t_n[0], vec![Some(5.0), None]);
        // best: d1=5, d2=10 -> 11 at T=10 (inclusive cap)
        assert_eq!(o.cum[1][10], 11.0);
        assert_eq!(o.t_n[1], vec![Some(5.0), Some(10.0)]);
        // meancount: at T=7 mean of (10, 10) = 10; at T=10 mean of (10, 11)
        assert_eq!(o.cum[2][7], 10.0);
        assert_eq!(o.cum[2][10], 10.5);
        assert_eq!(o.t_n[2], vec![Some(6.0), Some(10.0)]);
        assert_eq!(o.meancount_ndef, vec![0, 1]);
        // meantt: only d1 reachable at both departures, mean 6
        assert_eq!(o.cum[3][5], 0.0);
        assert_eq!(o.cum[3][6], 10.0);
        assert_eq!(o.cum[3][10], 10.0);
        // meanttcap: d1 mean 6; d2 (unreachable, 10) -> mean 10 -> counted at T=10
        assert_eq!(o.cum[4][9], 10.0);
        assert_eq!(o.cum[4][10], 11.0);
        assert_eq!(o.t_n[4], vec![Some(6.0), Some(10.0)]);
    }

    #[test]
    fn continuous_direct_walk_changes_only_t_n() {
        use crate::geo::{Grid, LatLon};
        // origin 0 and dest 1 are 600 m apart: 600 / (5000/3600) / 60 = 7.2 min (stream: 8)
        let pts = vec![LatLon { lat: 50.0, lon: 8.0 }, LatLon { lat: 50.0 + (600.0f64 / 6_371_000.0).to_degrees(), lon: 8.0 }];
        let d = crate::geo::haversine_m(pts[0], pts[1]);
        assert!((d - 600.0).abs() < 0.5, "{d}");
        let r = rec(0, vec![0, 8], vec![DIRECT_WALK, DIRECT_WALK]);
        let w = [1.0, 5.0];
        let c = Continuous { origins: &pts, dests: &pts, grid: Grid::new(&pts, 0.05), speed_mps: 5000.0 / 3600.0, max_m: 2000.0 };
        let p = Params { weights: &w, fixed_index: 0, cumulative: &[5.0], threads: 1, self_index: Some(vec![Some(0), Some(1)]), continuous: Some(c) };
        let o = reduce_record(&r, 2, 1, 10, &p).unwrap();
        assert_eq!(o.cum[0][7], 0.0);
        assert_eq!(o.cum[0][8], 5.0);
        let t = o.t_n[0][0].unwrap();
        assert!((t - d / (5000.0 / 3600.0) / 60.0).abs() < 1e-9, "{t}");
    }
}
