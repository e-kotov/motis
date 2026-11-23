//! VRTT travel-time stream (format version 1), written by `motis-accessibility route`
//! and by this crate's converters. Little-endian throughout.
//!
//! header (56 bytes): magic "VRTTMATX", u32 version, u32 n_origins_total, u32 n_dests,
//! u32 n_departures, u32 origin_from, u32 origin_to, u16 max_travel_minutes,
//! u8 flags (bit0 transfers block, bit1 egress-stop block), u8 target (0 dests, 1 stops),
//! 5 x u32 reserved; then n_departures x i64 departure (unix minutes, UTC).
//! records (any order): u32 origin; tt[n_departures][n_dests] as u8 (0..=max minutes,
//! 255 unreachable) or, with flag bit2 (`tt16`, required when max > 253), as u16
//! (65535 unreachable); optional u8 transfers[..] (0.., 253 walk via stop without vehicle, 254 direct walk, 255
//! unreachable); optional u32 egress_stop[..] (0xFFFFFFFF none).
//! In memory, travel times are always u16 with `UNREACHABLE` = 65535.

use anyhow::{bail, Context, Result};
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Write};
use std::path::Path;

pub const UNREACHABLE: u16 = u16::MAX;
pub const TR_UNREACHABLE: u8 = 255;
pub const DIRECT_WALK: u8 = 254;
pub const WALK_VIA_STOP: u8 = 253;
pub const NO_STOP: u32 = u32::MAX;
pub const FLAG_TRANSFERS: u8 = 1;
pub const FLAG_VIA: u8 = 2;
pub const FLAG_TT16: u8 = 4;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    pub n_origins_total: u32,
    pub n_dests: u32,
    pub n_departures: u32,
    pub origin_from: u32,
    pub origin_to: u32,
    pub max_travel: u16,
    pub flags: u8,
    pub target: u8,
    pub departures: Vec<i64>,
}

impl Header {
    pub fn block(&self) -> usize {
        self.n_departures as usize * self.n_dests as usize
    }
    pub fn has_transfers(&self) -> bool {
        self.flags & FLAG_TRANSFERS != 0
    }
    pub fn has_via(&self) -> bool {
        self.flags & FLAG_VIA != 0
    }
    pub fn tt16(&self) -> bool {
        self.flags & FLAG_TT16 != 0
    }
    pub fn record_bytes(&self) -> usize {
        let b = self.block();
        4 + if self.tt16() { 2 * b } else { b } + if self.has_transfers() { b } else { 0 } + if self.has_via() { 4 * b } else { 0 }
    }
}

pub struct Record {
    pub origin: u32,
    pub tt: Vec<u16>,
    pub transfers: Vec<u8>,
    pub via: Vec<u32>,
}

pub struct Reader {
    pub header: Header,
    r: BufReader<File>,
    buf: Vec<u8>,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

impl Reader {
    pub fn open(p: &Path) -> Result<Reader> {
        let f = File::open(p).with_context(|| format!("open {}", p.display()))?;
        let mut r = BufReader::with_capacity(1 << 22, f);
        let mut h = [0u8; 56];
        r.read_exact(&mut h).context("read header")?;
        if &h[0..8] != b"VRTTMATX" {
            bail!("{}: bad magic", p.display());
        }
        let version = u32_at(&h, 8);
        if version != 1 {
            bail!("{}: unsupported version {}", p.display(), version);
        }
        let n_departures = u32_at(&h, 20);
        let mut deps = Vec::with_capacity(n_departures as usize);
        for _ in 0..n_departures {
            let mut d = [0u8; 8];
            r.read_exact(&mut d)?;
            deps.push(i64::from_le_bytes(d));
        }
        let header = Header {
            n_origins_total: u32_at(&h, 12),
            n_dests: u32_at(&h, 16),
            n_departures,
            origin_from: u32_at(&h, 24),
            origin_to: u32_at(&h, 28),
            max_travel: u16::from_le_bytes([h[32], h[33]]),
            flags: h[34],
            target: h[35],
            departures: deps,
        };
        let buf = vec![0u8; header.record_bytes()];
        Ok(Reader { header, r, buf })
    }

    /// Returns false at a clean end of file.
    pub fn next_into(&mut self, rec: &mut Record) -> Result<bool> {
        let n = self.buf.len();
        let mut got = 0;
        while got < n {
            let k = self.r.read(&mut self.buf[got..])?;
            if k == 0 {
                if got == 0 {
                    return Ok(false);
                }
                bail!("truncated record ({} of {} bytes)", got, n);
            }
            got += k;
        }
        let b = self.header.block();
        rec.origin = u32_at(&self.buf, 0);
        rec.tt.clear();
        let mut o = 4;
        if self.header.tt16() {
            rec.tt.extend(self.buf[o..o + 2 * b].chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])));
            o += 2 * b;
        } else {
            rec.tt.extend(self.buf[o..o + b].iter().map(|&x| if x == 255 { UNREACHABLE } else { x as u16 }));
            o += b;
        }
        rec.transfers.clear();
        if self.header.has_transfers() {
            rec.transfers.extend_from_slice(&self.buf[o..o + b]);
            o += b;
        }
        rec.via.clear();
        if self.header.has_via() {
            rec.via.extend(self.buf[o..o + 4 * b].chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())));
        }
        Ok(true)
    }

    pub fn read_all(p: &Path) -> Result<(Header, Vec<Record>)> {
        let mut rd = Reader::open(p)?;
        let mut out = Vec::new();
        loop {
            let mut rec = Record { origin: 0, tt: vec![], transfers: vec![], via: vec![] };
            if !rd.next_into(&mut rec)? {
                break;
            }
            out.push(rec);
        }
        Ok((rd.header, out))
    }
}

pub struct Writer {
    pub header: Header,
    w: BufWriter<File>,
}

impl Writer {
    pub fn create(p: &Path, mut header: Header) -> Result<Writer> {
        if header.max_travel > 253 {
            header.flags |= FLAG_TT16;
        }
        let f = File::create(p).with_context(|| format!("create {}", p.display()))?;
        let mut w = BufWriter::with_capacity(1 << 22, f);
        let mut h = Vec::with_capacity(56);
        h.extend_from_slice(b"VRTTMATX");
        for v in [1u32, header.n_origins_total, header.n_dests, header.n_departures, header.origin_from, header.origin_to] {
            h.extend_from_slice(&v.to_le_bytes());
        }
        h.extend_from_slice(&header.max_travel.to_le_bytes());
        h.push(header.flags);
        h.push(header.target);
        h.extend_from_slice(&[0u8; 20]);
        assert_eq!(h.len(), 56);
        w.write_all(&h)?;
        for d in &header.departures {
            w.write_all(&d.to_le_bytes())?;
        }
        Ok(Writer { header, w })
    }

    pub fn write(&mut self, rec: &Record) -> Result<()> {
        let b = self.header.block();
        if rec.tt.len() != b {
            bail!("record tt size {} != {}", rec.tt.len(), b);
        }
        self.w.write_all(&rec.origin.to_le_bytes())?;
        if self.header.tt16() {
            for t in &rec.tt {
                self.w.write_all(&t.to_le_bytes())?;
            }
        } else {
            let v: Vec<u8> = rec.tt.iter().map(|&t| if t == UNREACHABLE { 255 } else { assert!(t <= 253); t as u8 }).collect();
            self.w.write_all(&v)?;
        }
        if self.header.has_transfers() {
            if rec.transfers.len() != b {
                bail!("record transfers size mismatch");
            }
            self.w.write_all(&rec.transfers)?;
        }
        if self.header.has_via() {
            if rec.via.len() != b {
                bail!("record via size mismatch");
            }
            for v in &rec.via {
                self.w.write_all(&v.to_le_bytes())?;
            }
        }
        Ok(())
    }

    pub fn finish(mut self) -> Result<()> {
        self.w.flush()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(cap: u16, tt: Vec<u16>) -> (Header, Vec<u16>) {
        let dir = std::env::temp_dir().join(format!("vrtt-test-{}-{}", std::process::id(), cap));
        let h = Header { n_origins_total: 1, n_dests: tt.len() as u32, n_departures: 1, origin_from: 0, origin_to: 1, max_travel: cap, flags: FLAG_TRANSFERS, target: 0, departures: vec![42] };
        let mut w = Writer::create(&dir, h).unwrap();
        w.write(&Record { origin: 0, tt: tt.clone(), transfers: vec![0; tt.len()], via: vec![] }).unwrap();
        w.finish().unwrap();
        let (h2, recs) = Reader::read_all(&dir).unwrap();
        std::fs::remove_file(&dir).unwrap();
        (h2, recs[0].tt.clone())
    }

    #[test]
    fn long_horizon_is_lossless() {
        let (h, tt) = roundtrip(300, vec![0, 253, 254, 299, 300, UNREACHABLE]);
        assert!(h.tt16());
        assert_eq!(tt, vec![0, 253, 254, 299, 300, UNREACHABLE]);
        let (h, tt) = roundtrip(95, vec![0, 95, UNREACHABLE]);
        assert!(!h.tt16());
        assert_eq!(tt, vec![0, 95, UNREACHABLE]);
    }
}
