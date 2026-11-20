//! DTS APO4 (DTS:X Ultra and similar) speaker EQ from a Windows install.
//!
//! OEMs ship it as `Windows/System32/DTS/PC/APO4x/dts_gpeq_mode<N>_48k_<SUBSYS>.bin`, one file per
//! operating mode, named in `dts_apo4_oem_config_<SUBSYS>.xml`. A file is a little-endian `i32`
//! count (twice the number of biquads) and an `i32` zero, then 24-byte records: type 2, then
//! b0 b1 b2 a1 a2 in Q30. The recursion is `y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2`.
//! The second half of the file repeats the set; it is identical in every Acer file seen, so the first
//! set serves both channels. Only this EQ is reproduced: the DTS processing (`TruBass`, dialogue
//! enhancement, spatial) lives in a licensed DLL and is not a filter.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};

use crate::{Biquad, Endpoint, Profile, Tuning, sonitude};

/// Files are about 5 KiB; a larger one is not a tuning.
const MAX_BYTES: u64 = 1 << 20;
const Q30: f64 = 1_073_741_824.0;
const RECORD: usize = 24;
const BIQUAD: i32 = 2;
/// Style in Settings → the file's mode number (`APO4-Music`, `-Movie`, `-Voice`, `-Game1`).
const STYLES: [(&str, u32); 4] = [("music", 1), ("movie", 2), ("voice", 3), ("game", 4)];

/// The biquads one `dts_gpeq_*` file holds, for both channels; empty for an "off" file.
pub fn read_gpeq(d: &[u8]) -> Result<Vec<Biquad>> {
    let word = |i: usize| -> Result<i32> {
        let b = d.get(i * 4..i * 4 + 4).context("file is cut short")?;
        Ok(i32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    let count = usize::try_from(word(0)?).context("negative biquad count")?;
    if count % 2 != 0 || 8 + count / 2 * RECORD > d.len() {
        bail!("header says {count} biquads in a {}-byte file", d.len());
    }
    let mut out = Vec::new();
    for n in 0..count / 2 {
        let at = 2 + n * RECORD / 4;
        if word(at)? != BIQUAD {
            bail!("record {n} has type {}, not a biquad", word(at)?);
        }
        let mut c = [0.0; 5];
        for (k, v) in c.iter_mut().enumerate() {
            *v = f64::from(word(at + 1 + k)?) / Q30;
        }
        // Poles inside the unit circle: |a2| < 1 and |a1| < 1 - a2 (the denominator is 1 - a1 z^-1 - a2 z^-2).
        if c[4].abs() >= 1.0 || c[3].abs() >= 1.0 - c[4] {
            bail!("record {n} is an unstable filter");
        }
        // Ours is 1 + a1 z^-1 + a2 z^-2.
        let coeffs = [c[0], c[1], c[2], -c[3], -c[4]];
        out.extend((0..2).map(|speaker| Biquad { speaker, coeffs }));
    }
    Ok(out)
}

/// The tuning `dir` holds for codec `subsystem`; `None` when it has no 48 kHz speaker files for it.
pub fn read_dir(dir: &Path, subsystem: &str) -> Result<Option<Tuning>> {
    let mut profiles = BTreeMap::new();
    for (style, mode) in STYLES {
        let name = format!("dts_gpeq_mode{mode}_48k_{subsystem}.bin");
        let Some(path) = fs::read_dir(dir)?
            .flatten()
            .map(|e| e.path())
            .find(|p| p.file_name().is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case(&name)))
        else {
            continue;
        };
        if fs::metadata(&path)?.len() > MAX_BYTES {
            bail!("{} is larger than a tuning file", path.display());
        }
        let bytes = fs::read(&path).with_context(|| format!("read {}", path.display()))?;
        let biquads = read_gpeq(&bytes).with_context(|| path.display().to_string())?;
        profiles.insert(style.to_owned(), Profile { biquads, ..Profile::default() });
    }
    if !profiles.contains_key("music") {
        return Ok(None);
    }
    Ok(Some(Tuning {
        source: "DTS".to_owned(),
        subsystem: subsystem.to_owned(),
        tuning_date: String::new(),
        // The fixed biquads are fitted with bells on this grid, as for Surface.
        band_freqs_hz: sonitude::band_centres(),
        ieq_presets: BTreeMap::new(),
        endpoints: vec![Endpoint { kind: "internal_speaker".to_owned(), channels: 2, profiles }],
    }))
}

/// The DTS folders of a Windows install: `Windows/System32/DTS/<product>/<apo>`.
pub fn folders(root: &Path) -> Vec<PathBuf> {
    let subdirs = |d: &Path| -> Vec<PathBuf> {
        fs::read_dir(d).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect()
    };
    subdirs(&root.join("Windows/System32/DTS")).iter().flat_map(|p| subdirs(p)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::chain::{channel_eq, peaking_db};

    #[allow(clippy::cast_possible_truncation)]
    fn q30(v: f64) -> i32 {
        (v * Q30) as i32
    }

    /// A gpeq file of the given (b0, b1, b2, a1, a2) records, plus the unity gain record real files hold.
    fn gpeq(records: &[[f64; 5]]) -> Vec<u8> {
        let mut w = vec![0, 0];
        for r in records {
            w[0] += 2;
            w.push(BIQUAD);
            w.extend(r.map(q30));
        }
        w.extend([1, i32::MAX, 0, 0, 0, 0]);
        w.iter().flat_map(|v| v.to_le_bytes()).collect()
    }

    /// RBJ peaking at 48 kHz in the file's sign convention (a1, a2 positive feedback).
    fn peaking(f0: f64, q: f64, gain_db: f64) -> [f64; 5] {
        let a = 10f64.powf(gain_db / 40.0);
        let w = 2.0 * std::f64::consts::PI * f0 / 48_000.0;
        let al = w.sin() / (2.0 * q);
        let a0 = 1.0 + al / a;
        [(1.0 + al * a) / a0, -2.0 * w.cos() / a0, (1.0 - al * a) / a0, 2.0 * w.cos() / a0, -(1.0 - al / a) / a0]
    }

    #[test]
    fn reads_records_for_both_channels() -> Result<()> {
        let bq = read_gpeq(&gpeq(&[peaking(1000.0, 1.0, -6.0)]))?;
        assert_eq!(bq.len(), 2);
        assert_eq!((bq[0].speaker, bq[1].speaker), (0, 1));
        assert!((bq[0].coeffs[3] + peaking(1000.0, 1.0, -6.0)[3]).abs() < 1e-6, "feedback sign flips");
        Ok(())
    }

    #[test]
    fn off_file_is_empty_and_garbage_is_refused() -> Result<()> {
        assert_eq!(read_gpeq(&gpeq(&[]))?, Vec::<Biquad>::new());
        assert!(read_gpeq(&[1, 2, 3]).is_err());
        let mut bad = gpeq(&[peaking(1000.0, 1.0, -6.0)]);
        bad[8] = 7;
        assert!(read_gpeq(&bad).is_err(), "unknown record type");
        let unstable = gpeq(&[[1.0, 0.0, 0.0, 1.5, 0.0]]);
        assert!(read_gpeq(&unstable).is_err());
        Ok(())
    }

    #[test]
    fn folder_becomes_music_profile_that_shapes_the_chain() -> Result<()> {
        let dir = tempfile::tempdir()?;
        fs::write(dir.path().join("DTS_GPEQ_MODE1_48K_1025171E.BIN"), gpeq(&[peaking(1000.0, 1.0, -6.0)]))?;
        assert!(read_dir(dir.path(), "10431A63")?.is_none());
        let t = read_dir(dir.path(), "1025171E")?.context("tuning")?;
        let p = &t.endpoints[0].profiles["music"];
        let at = |f: f64| channel_eq(&t, p, 0).iter().map(|b| peaking_db(b.freq, b.q, b.gain_db, f)).sum::<f64>();
        assert!((at(1000.0) + 6.0).abs() < 1.0, "1 kHz: {}", at(1000.0));
        assert!(at(8000.0).abs() < 1.0, "8 kHz: {}", at(8000.0));
        Ok(())
    }
}
