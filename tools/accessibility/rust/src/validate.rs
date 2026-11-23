//! Output gate for VRTT streams: header/value invariants, origin coverage, transfer
//! coverage and a canonical (thread-order independent) digest.

use crate::geo::read_connectors;
use crate::stream::{Reader, Record, DIRECT_WALK, TR_UNREACHABLE, UNREACHABLE, WALK_VIA_STOP};
use anyhow::Result;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::Path;

pub struct Expect {
    pub origins: Option<Vec<u32>>,
    pub max_transfers: u8,
    pub allow_walk_via_stop: bool,
    pub require_transfers: bool,
    pub direct: Option<HashMap<(u32, u32), u16>>,
}

pub fn read_direct(p: &Path) -> Result<HashMap<(u32, u32), u16>> {
    Ok(read_connectors(p)?.into_iter().map(|(o, d, m)| ((o, d), m)).collect())
}

/// Returns a JSON report; `errors` lists the first violations (empty = valid).
pub fn validate(stream: &Path, ex: &Expect) -> Result<serde_json::Value> {
    let mut rd = Reader::open(stream)?;
    let h = rd.header.clone();
    let mut errors: Vec<String> = Vec::new();
    let mut err = |e: String, errors: &mut Vec<String>| {
        if errors.len() < 50 {
            errors.push(e);
        }
    };
    if h.n_departures as usize != h.departures.len() {
        err("departure count mismatch".into(), &mut errors);
    }
    if h.origin_from > h.origin_to || h.origin_to > h.n_origins_total {
        err(format!("bad origin range [{}, {}) of {}", h.origin_from, h.origin_to, h.n_origins_total), &mut errors);
    }
    if h.target > 1 {
        err(format!("bad target {}", h.target), &mut errors);
    }
    if ex.require_transfers && !h.has_transfers() {
        err("transfers block required but absent".into(), &mut errors);
    }
    let cap = h.max_travel;
    let nd = h.n_dests as usize;
    let mut seen: HashSet<u32> = HashSet::new();
    let mut digests: BTreeMap<u32, [u8; 32]> = BTreeMap::new();
    let mut rec = Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
    let (mut reach, mut n_rec) = (0u64, 0u64);
    let mut tr_hist: BTreeMap<u8, u64> = BTreeMap::new();
    while rd.next_into(&mut rec)? {
        n_rec += 1;
        let o = rec.origin;
        if !seen.insert(o) {
            err(format!("duplicate origin {}", o), &mut errors);
        }
        if o >= h.n_origins_total {
            err(format!("origin {} >= n_origins_total {}", o, h.n_origins_total), &mut errors);
        }
        for i in 0..rec.tt.len() {
            let t = rec.tt[i];
            let d = (i % nd) as u32;
            if t != UNREACHABLE && t > cap {
                err(format!("origin {} cell {} value {} above cap {}", o, i, t, cap), &mut errors);
            }
            if t != UNREACHABLE {
                reach += 1;
            }
            if let Some(&k) = rec.transfers.get(i) {
                *tr_hist.entry(k).or_default() += 1;
                if (k == TR_UNREACHABLE) != (t == UNREACHABLE) {
                    err(format!("origin {} cell {} tt {} transfers {} disagree on reachability", o, i, t, k), &mut errors);
                }
                if k != TR_UNREACHABLE {
                    if h.target == 1 {
                        // stops stream: k = vehicles used, 0..=max_transfers+1
                        if k as u16 > ex.max_transfers as u16 + 1 {
                            err(format!("origin {} stop {} k {} above max_transfers+1", o, d, k), &mut errors);
                        }
                    } else if k == DIRECT_WALK {
                        match &ex.direct {
                            Some(dm) => match dm.get(&(o, d)) {
                                Some(&m) if m == t => {}
                                other => err(format!("origin {} dest {} coded direct walk {} but direct connector is {:?}", o, d, t, other), &mut errors),
                            },
                            None => {}
                        }
                    } else if k == WALK_VIA_STOP {
                        if !ex.allow_walk_via_stop {
                            err(format!("origin {} dest {}: walk via stop (no vehicle) in transit-egress output", o, d), &mut errors);
                        }
                    } else if k > ex.max_transfers {
                        err(format!("origin {} dest {} transfers {} above max {}", o, d, k, ex.max_transfers), &mut errors);
                    }
                }
            }
        }
        let mut hs = Sha256::new();
        hs.update(o.to_le_bytes());
        for t in &rec.tt {
            hs.update(t.to_le_bytes());
        }
        hs.update(&rec.transfers);
        digests.insert(o, hs.finalize().into());
    }
    if let Some(exp) = &ex.origins {
        let e: HashSet<u32> = exp.iter().copied().collect();
        let missing: Vec<u32> = e.difference(&seen).copied().take(20).collect();
        let extra: Vec<u32> = seen.difference(&e).copied().take(20).collect();
        if !missing.is_empty() || !extra.is_empty() {
            err(format!("origin set mismatch: missing {:?} extra {:?}", missing, extra), &mut errors);
        }
    }
    let mut canon = Sha256::new();
    for (_, d) in &digests {
        canon.update(d);
    }
    Ok(serde_json::json!({
        "valid": errors.is_empty(), "errors": errors, "records": n_rec, "reachable_cells": reach,
        "transfer_code_histogram": tr_hist, "max_travel": cap, "target": h.target, "flags": h.flags,
        "canonical_digest": format!("{:x}", canon.finalize()),
    }))
}
