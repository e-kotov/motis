//! Paired straight-line vs directed street-walk comparison: connector identity/minutes
//! and per-origin accessibility differences between two `reduce` outputs.

use crate::geo::{read_connectors, Table};
use anyhow::{bail, Result};
use std::collections::{BTreeMap, HashMap};
use std::io::Write;
use std::path::Path;

/// Per point: stops reachable in A and B, shared stops, mean/max |minutes diff| on shared,
/// and the nearest-stop (min minutes) identity agreement.
pub fn connector_compare(a: &Path, b: &Path, out: &Path) -> Result<serde_json::Value> {
    let load = |p: &Path| -> Result<BTreeMap<u32, HashMap<u32, u16>>> {
        let mut m: BTreeMap<u32, HashMap<u32, u16>> = BTreeMap::new();
        for (pt, s, mins) in read_connectors(p)? {
            let e = m.entry(pt).or_default().entry(s).or_insert(mins);
            *e = (*e).min(mins);
        }
        Ok(m)
    };
    let (ma, mb) = (load(a)?, load(b)?);
    let pts: std::collections::BTreeSet<u32> = ma.keys().chain(mb.keys()).copied().collect();
    let mut w = std::io::BufWriter::new(std::fs::File::create(out)?);
    writeln!(w, "point,n_a,n_b,shared,mean_diff_b_minus_a,max_abs_diff,nearest_a,nearest_b,nearest_a_min,nearest_b_min")?;
    let empty = HashMap::new();
    let (mut tot_a, mut tot_b, mut tot_sh, mut same_nearest, mut both_any, mut only_a_pts, mut only_b_pts) = (0u64, 0u64, 0u64, 0u64, 0u64, 0u64, 0u64);
    let mut diff_hist: BTreeMap<i32, u64> = BTreeMap::new();
    for p in &pts {
        let (ca, cb) = (ma.get(p).unwrap_or(&empty), mb.get(p).unwrap_or(&empty));
        let mut sh = 0u64;
        let (mut sum, mut mx) = (0i64, 0i64);
        for (s, &x) in ca {
            if let Some(&y) = cb.get(s) {
                sh += 1;
                let d = y as i64 - x as i64;
                sum += d;
                mx = mx.max(d.abs());
                *diff_hist.entry(d as i32).or_default() += 1;
            }
        }
        let near = |c: &HashMap<u32, u16>| c.iter().min_by_key(|(s, m)| (**m, **s)).map(|(s, m)| (*s, *m));
        let (na, nb) = (near(ca), near(cb));
        match (na, nb) {
            (Some(x), Some(y)) => {
                both_any += 1;
                if x.0 == y.0 {
                    same_nearest += 1;
                }
            }
            (Some(_), None) => only_a_pts += 1,
            (None, Some(_)) => only_b_pts += 1,
            _ => {}
        }
        tot_a += ca.len() as u64;
        tot_b += cb.len() as u64;
        tot_sh += sh;
        let f = |o: Option<(u32, u16)>, k: usize| o.map(|v| if k == 0 { v.0.to_string() } else { v.1.to_string() }).unwrap_or_default();
        writeln!(w, "{},{},{},{},{},{},{},{},{},{}", p, ca.len(), cb.len(), sh, if sh > 0 { format!("{:.3}", sum as f64 / sh as f64) } else { String::new() }, mx, f(na, 0), f(nb, 0), f(na, 1), f(nb, 1))?;
    }
    w.flush()?;
    Ok(serde_json::json!({
        "points": pts.len(), "connectors_a": tot_a, "connectors_b": tot_b, "shared": tot_sh,
        "points_with_stops_both": both_any, "points_only_a": only_a_pts, "points_only_b": only_b_pts,
        "same_nearest_stop": same_nearest, "minutes_diff_b_minus_a_histogram": diff_hist,
    }))
}

/// Per origin difference (B - A) of c_T for one semantics; population-weighted totals of
/// origins changing class across thresholds (reachable population >= `levels`).
pub fn summary_diff(a: &Path, b: &Path, origins: &Path, weight_col: &str, semantics: &str, ts: &[usize], levels: &[f64], out: &Path) -> Result<serde_json::Value> {
    let read = |p: &Path| -> Result<(Vec<String>, HashMap<u32, Vec<String>>)> {
        let t = Table::read(p)?;
        let (co, cs) = (t.col("origin")?, t.col("semantics")?);
        let mut m = HashMap::new();
        for r in &t.rows {
            if &r[cs] == semantics {
                m.insert(r[co].parse()?, r.iter().map(|x| x.to_string()).collect());
            }
        }
        Ok((t.header.clone(), m))
    };
    let (ha, ma) = read(a)?;
    let (hb, mb) = read(b)?;
    if ha != hb {
        bail!("summary headers differ");
    }
    let ot = Table::read(origins)?;
    let wc = ot.col(weight_col)?;
    let w: Vec<f64> = ot.rows.iter().map(|r| r[wc].parse().unwrap_or(0.0)).collect();
    let idx: Vec<usize> = ts.iter().map(|t| ha.iter().position(|h| h == &format!("c_{}", t)).ok_or_else(|| anyhow::anyhow!("no c_{}", t))).collect::<Result<_>>()?;
    let mut f = std::io::BufWriter::new(std::fs::File::create(out)?);
    write!(f, "origin,weight")?;
    for t in ts {
        write!(f, ",a_c_{t},b_c_{t},diff_c_{t}")?;
    }
    writeln!(f)?;
    let mut keys: Vec<u32> = ma.keys().copied().filter(|k| mb.contains_key(k)).collect();
    keys.sort();
    let mut per_t = Vec::new();
    for (j, t) in ts.iter().enumerate() {
        let (mut sa, mut sb, mut wsum, mut changed_w, mut down_w, mut up_w) = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        let mut class_change = vec![(0.0f64, 0.0f64); levels.len()];
        for &o in &keys {
            let x: f64 = ma[&o][idx[j]].parse()?;
            let y: f64 = mb[&o][idx[j]].parse()?;
            let wt = w[o as usize];
            sa += wt * x;
            sb += wt * y;
            wsum += wt;
            if x != y {
                changed_w += wt;
                if y < x { down_w += wt } else { up_w += wt }
            }
            for (k, &l) in levels.iter().enumerate() {
                if x >= l && y < l {
                    class_change[k].0 += wt;
                }
                if x < l && y >= l {
                    class_change[k].1 += wt;
                }
            }
        }
        per_t.push(serde_json::json!({
            "T": t, "weighted_mean_a": sa / wsum, "weighted_mean_b": sb / wsum, "relative_change": (sb - sa) / sa,
            "weight_with_any_change": changed_w, "weight_lower_in_b": down_w, "weight_higher_in_b": up_w,
            "class_changes": levels.iter().zip(class_change.iter()).map(|(l, c)| serde_json::json!({"level": l, "weight_drop_below": c.0, "weight_rise_above": c.1})).collect::<Vec<_>>(),
        }));
    }
    for &o in &keys {
        write!(f, "{},{}", o, w[o as usize])?;
        for j in 0..ts.len() {
            let x: f64 = ma[&o][idx[j]].parse()?;
            let y: f64 = mb[&o][idx[j]].parse()?;
            write!(f, ",{},{},{}", x, y, y - x)?;
        }
        writeln!(f)?;
    }
    f.flush()?;
    Ok(serde_json::json!({"semantics": semantics, "origins": keys.len(), "weight_total": w.iter().sum::<f64>(), "per_threshold": per_t}))
}
