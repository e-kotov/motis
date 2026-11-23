//! Typed comparison of two VRTT streams (cell-by-cell travel times, transfer counts).

use crate::stream::{Reader, Record, DIRECT_WALK, UNREACHABLE};
use anyhow::{bail, Result};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap};
use std::path::Path;

#[derive(Serialize, Default)]
pub struct Report {
    pub origins_compared: u64,
    pub duplicate_origins_in_a: Vec<u32>,
    pub transfers_present_a: bool,
    pub transfers_present_b: bool,
    pub origins_only_in_b: Vec<u32>,
    pub cells_compared: u64,
    pub equal: u64,
    pub both_unreachable: u64,
    pub reachable_equal: u64,
    pub only_a_reachable: u64,
    pub only_b_reachable: u64,
    pub both_reachable_diff: u64,
    pub max_abs_diff: u32,
    pub diff_histogram: BTreeMap<i32, u64>,
    pub transfers_compared: u64,
    pub transfers_equal: u64,
    pub transfers_mode_mismatch: u64,
    pub first_mismatches: Vec<Mismatch>,
    pub first_transfer_mismatches: Vec<Mismatch>,
    /// travel-time mismatches per "origin:departure_index"
    pub mismatch_groups: BTreeMap<String, u64>,
}

#[derive(Serialize, Clone)]
pub struct Mismatch {
    pub origin: u32,
    pub departure_index: u32,
    pub dest: u32,
    pub a_tt: u16,
    pub b_tt: u16,
    pub a_transfers: Option<u8>,
    pub b_transfers: Option<u8>,
    pub a_egress_stop: Option<u32>,
}

/// Loads B completely (it must be the smaller, sampled side), streams A.
pub fn compare(a: &Path, b: &Path, max_report: usize, cap: Option<u16>) -> Result<Report> {
    let (hb, recs_b) = Reader::read_all(b)?;
    let mut ra = Reader::open(a)?;
    let ha = ra.header.clone();
    if ha.n_dests != hb.n_dests || ha.departures != hb.departures || ha.target != hb.target {
        bail!("streams are not comparable (n_dests {}/{}, departures equal: {}, target {}/{})", ha.n_dests, hb.n_dests, ha.departures == hb.departures, ha.target, hb.target);
    }
    let cap = cap.unwrap_or(ha.max_travel.min(hb.max_travel));
    let by_origin: HashMap<u32, &Record> = recs_b.iter().map(|r| (r.origin, r)).collect();
    if by_origin.len() != recs_b.len() {
        bail!("B has duplicate origins");
    }
    let nd = ha.n_dests as usize;
    let mut rep = Report::default();
    let mut seen = std::collections::HashSet::new();
    let mut seen_a = std::collections::HashSet::new();
    rep.transfers_present_a = ha.has_transfers();
    rep.transfers_present_b = hb.has_transfers();
    let mut rec = Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
    let norm = |t: u16| if t != UNREACHABLE && t > cap { UNREACHABLE } else { t };
    while ra.next_into(&mut rec)? {
        if !seen_a.insert(rec.origin) {
            rep.duplicate_origins_in_a.push(rec.origin);
        }
        let Some(rb) = by_origin.get(&rec.origin) else { continue };
        seen.insert(rec.origin);
        rep.origins_compared += 1;
        for i in 0..rec.tt.len() {
            let (x, y) = (norm(rec.tt[i]), norm(rb.tt[i]));
            rep.cells_compared += 1;
            let mk = || Mismatch {
                origin: rec.origin,
                departure_index: (i / nd) as u32,
                dest: (i % nd) as u32,
                a_tt: rec.tt[i],
                b_tt: rb.tt[i],
                a_transfers: rec.transfers.get(i).copied(),
                b_transfers: rb.transfers.get(i).copied(),
                a_egress_stop: rec.via.get(i).copied(),
            };
            if x == y {
                rep.equal += 1;
                if x == UNREACHABLE {
                    rep.both_unreachable += 1;
                } else {
                    rep.reachable_equal += 1;
                    if !rec.transfers.is_empty() && !rb.transfers.is_empty() {
                        rep.transfers_compared += 1;
                        let (ta, tb) = (rec.transfers[i], rb.transfers[i]);
                        if ta == tb {
                            rep.transfers_equal += 1;
                        } else {
                            if (ta == DIRECT_WALK) != (tb == DIRECT_WALK) {
                                rep.transfers_mode_mismatch += 1;
                            }
                            if rep.first_transfer_mismatches.len() < max_report {
                                rep.first_transfer_mismatches.push(mk());
                            }
                        }
                    }
                }
            } else {
                if x == UNREACHABLE {
                    rep.only_b_reachable += 1;
                } else if y == UNREACHABLE {
                    rep.only_a_reachable += 1;
                } else {
                    rep.both_reachable_diff += 1;
                    let d = x as i32 - y as i32;
                    *rep.diff_histogram.entry(d).or_default() += 1;
                    rep.max_abs_diff = rep.max_abs_diff.max(d.unsigned_abs());
                }
                *rep.mismatch_groups.entry(format!("{}:{}", rec.origin, i / nd)).or_default() += 1;
                if rep.first_mismatches.len() < max_report {
                    rep.first_mismatches.push(mk());
                }
            }
        }
    }
    rep.origins_only_in_b = recs_b.iter().map(|r| r.origin).filter(|o| !seen.contains(o)).collect();
    Ok(rep)
}

impl Report {
    pub fn tt_identical(&self) -> bool {
        self.cells_compared > 0 && self.equal == self.cells_compared && self.origins_only_in_b.is_empty() && self.duplicate_origins_in_a.is_empty()
    }
    /// strict transfer agreement: both sides carry transfers and agree on every reachable cell
    pub fn transfers_identical(&self) -> bool {
        self.transfers_present_a && self.transfers_present_b && self.transfers_compared == self.reachable_equal && self.transfers_equal == self.transfers_compared
    }
}
