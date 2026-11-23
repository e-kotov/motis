//! Points, stops and straight-line connectors.

use anyhow::{bail, Context, Result};
use std::collections::HashMap;
use std::io::Write;
use std::path::Path;

pub const EARTH_RADIUS_M: f64 = 6_371_000.0;

#[derive(Clone, Copy, Debug)]
pub struct LatLon {
    pub lat: f64,
    pub lon: f64,
}

/// Great-circle (haversine) distance in metres on a sphere of radius 6,371 km.
pub fn haversine_m(a: LatLon, b: LatLon) -> f64 {
    let (p1, p2) = (a.lat.to_radians(), b.lat.to_radians());
    let dp = p2 - p1;
    let dl = (b.lon - a.lon).to_radians();
    let h = (dp / 2.0).sin().powi(2) + p1.cos() * p2.cos() * (dl / 2.0).sin().powi(2);
    2.0 * EARTH_RADIUS_M * h.sqrt().min(1.0).asin()
}

/// Walking minutes, rounded up like MOTIS street offsets (ceil(seconds / 60)).
pub fn walk_minutes(dist_m: f64, speed_mps: f64) -> u16 {
    let s = dist_m / speed_mps;
    // guard against floating noise on exact multiples
    ((s / 60.0) - 1e-9).ceil().max(0.0) as u16
}

pub struct Table {
    pub header: Vec<String>,
    pub rows: Vec<csv::StringRecord>,
}

impl Table {
    pub fn read(p: &Path) -> Result<Table> {
        let mut r = csv::ReaderBuilder::new()
            .has_headers(true)
            .from_path(p)
            .with_context(|| format!("open {}", p.display()))?;
        let header = r.headers()?.iter().map(|s| s.to_string()).collect();
        let rows = r.records().collect::<std::result::Result<Vec<_>, _>>()?;
        Ok(Table { header, rows })
    }
    pub fn col(&self, name: &str) -> Result<usize> {
        self.header
            .iter()
            .position(|h| h == name)
            .with_context(|| format!("column {} missing (have {:?})", name, self.header))
    }
}

pub fn read_points(p: &Path) -> Result<Vec<LatLon>> {
    let t = Table::read(p)?;
    let (la, lo) = (t.col("lat")?, t.col("lon")?);
    t.rows
        .iter()
        .map(|r| Ok(LatLon { lat: r[la].parse()?, lon: r[lo].parse()? }))
        .collect()
}

pub fn read_weights(p: &Path, col: &str) -> Result<Vec<f64>> {
    let t = Table::read(p)?;
    let c = t.col(col)?;
    t.rows.iter().map(|r| Ok(r[c].parse::<f64>()?)).collect()
}

pub struct Stop {
    pub idx: u32,
    pub id: String,
    pub pos: LatLon,
    pub traffic: bool,
}

pub fn read_stops(p: &Path) -> Result<Vec<Stop>> {
    let t = Table::read(p)?;
    let (s, id, la, lo, tr) = (t.col("stop")?, t.col("id")?, t.col("lat")?, t.col("lon")?, t.col("traffic")?);
    let mut out = Vec::with_capacity(t.rows.len());
    for (i, r) in t.rows.iter().enumerate() {
        let idx: u32 = r[s].parse()?;
        if idx as usize != i {
            bail!("stops.csv must be dense and ordered (row {} has stop {})", i, idx);
        }
        out.push(Stop { idx, id: r[id].to_string(), pos: LatLon { lat: r[la].parse()?, lon: r[lo].parse()? }, traffic: &r[tr] == "1" });
    }
    Ok(out)
}

/// Uniform lat/lon bucket grid for radius queries.
pub struct Grid {
    cell_deg: f64,
    buckets: HashMap<(i32, i32), Vec<u32>>,
}

impl Grid {
    pub fn new(points: &[LatLon], cell_deg: f64) -> Grid {
        let mut buckets: HashMap<(i32, i32), Vec<u32>> = HashMap::new();
        for (i, p) in points.iter().enumerate() {
            buckets.entry(Self::key(p, cell_deg)).or_default().push(i as u32);
        }
        Grid { cell_deg, buckets }
    }
    fn key(p: &LatLon, c: f64) -> (i32, i32) {
        ((p.lat / c).floor() as i32, (p.lon / c).floor() as i32)
    }
    /// Calls f(idx) for every candidate within `radius_m` (superset; caller filters).
    pub fn candidates(&self, p: &LatLon, radius_m: f64, mut f: impl FnMut(u32)) {
        let dlat = radius_m / 111_000.0;
        let dlon = radius_m / (111_000.0 * p.lat.to_radians().cos().max(0.05));
        let (a0, a1) = (((p.lat - dlat) / self.cell_deg).floor() as i32, ((p.lat + dlat) / self.cell_deg).floor() as i32);
        let (b0, b1) = (((p.lon - dlon) / self.cell_deg).floor() as i32, ((p.lon + dlon) / self.cell_deg).floor() as i32);
        for a in a0..=a1 {
            for b in b0..=b1 {
                if let Some(v) = self.buckets.get(&(a, b)) {
                    for &i in v {
                        f(i);
                    }
                }
            }
        }
    }
}

/// point,stop,minutes rows for all pairs within max_m (inclusive).
pub fn straight_pairs(from: &[LatLon], to: &[LatLon], to_ok: &dyn Fn(u32) -> bool, max_m: f64, speed_mps: f64) -> Vec<(u32, u32, u16)> {
    let grid = Grid::new(to, 0.05);
    let mut out = Vec::new();
    for (i, p) in from.iter().enumerate() {
        let mut row = Vec::new();
        grid.candidates(p, max_m, |j| {
            if !to_ok(j) {
                return;
            }
            let d = haversine_m(*p, to[j as usize]);
            if d <= max_m {
                row.push((j, walk_minutes(d, speed_mps)));
            }
        });
        row.sort_unstable();
        out.extend(row.into_iter().map(|(j, m)| (i as u32, j, m)));
    }
    out
}

pub fn write_connectors(p: &Path, rows: &[(u32, u32, u16)]) -> Result<()> {
    let mut w = std::io::BufWriter::new(std::fs::File::create(p)?);
    writeln!(w, "point,stop,minutes")?;
    for (a, b, m) in rows {
        writeln!(w, "{},{},{}", a, b, m)?;
    }
    w.flush()?;
    Ok(())
}

pub fn read_connectors(p: &Path) -> Result<Vec<(u32, u32, u16)>> {
    let t = Table::read(p)?;
    if t.header != ["point", "stop", "minutes"] {
        bail!("{}: expected header point,stop,minutes", p.display());
    }
    t.rows.iter().map(|r| Ok((r[0].parse()?, r[1].parse()?, r[2].parse()?))).collect()
}
