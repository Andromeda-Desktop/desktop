use crate::biquad::{Biquad, Coeffs};
use crate::{coef, db_to_lin};

pub const BANDS: usize = 4;
/// Gains are recomputed once per this many samples and ramped linearly between.
const STEP: usize = 32;
const BUTTERWORTH_Q: f32 = std::f32::consts::FRAC_1_SQRT_2;
/// Below this band level (dB) the band is quiet or silent: it gets no boost.
const GATE_DB: f32 = -60.0;
/// RMS window of the band level detector.
const RMS_MS: f32 = 10.0;

/// Fourth-order Linkwitz-Riley split: two cascaded Butterworth sections per side.
#[derive(Clone, Copy)]
struct Split {
    lp: [Biquad; 2],
    hp: [Biquad; 2],
}

impl Split {
    fn new(fs: f32, hz: f32) -> Self {
        Self {
            lp: [Biquad::new(Coeffs::lowpass(fs, hz, BUTTERWORTH_Q)); 2],
            hp: [Biquad::new(Coeffs::highpass(fs, hz, BUTTERWORTH_Q)); 2],
        }
    }

    fn run(&mut self, x: f32) -> (f32, f32) {
        let lo = self.lp[0].run(x);
        let lo = self.lp[1].run(lo);
        let hi = self.hp[0].run(x);
        let hi = self.hp[1].run(hi);
        (lo, hi)
    }
}

/// One channel's crossover tree. The low pair passes an all-pass at the top split and the high
/// pair one at the bottom split, so the four bands sum back to an all-pass of the input.
#[derive(Clone, Copy)]
struct Tree {
    mid: Split,
    low: Split,
    high: Split,
    low_ap: Biquad,
    high_ap: Biquad,
}

impl Tree {
    fn new(fs: f32, hz: [f32; 3]) -> Self {
        Self {
            low: Split::new(fs, hz[0]),
            mid: Split::new(fs, hz[1]),
            high: Split::new(fs, hz[2]),
            low_ap: Biquad::new(Coeffs::allpass(fs, hz[2], BUTTERWORTH_Q)),
            high_ap: Biquad::new(Coeffs::allpass(fs, hz[0], BUTTERWORTH_Q)),
        }
    }

    fn run(&mut self, x: f32) -> [f32; BANDS] {
        let (lo, hi) = self.mid.run(x);
        let (b0, b1) = self.low.run(self.low_ap.run(lo));
        let (b2, b3) = self.high.run(self.high_ap.run(hi));
        [b0, b1, b2, b3]
    }
}

/// Per-band settings; levels in dB.
#[derive(Clone, Copy)]
pub struct BandSettings {
    pub threshold_db: f32,
    /// Fraction of the level above the threshold that is removed (0 = off, 0.5 = 2:1).
    pub slope: f32,
    /// Upward gain for a band below the threshold, at most this many dB.
    pub boost_db: f32,
    pub attack_ms: f32,
    pub release_ms: f32,
}

/// Four-band compressor: the band above its threshold is pulled down by `slope`, the band below
/// is lifted by the same slope up to `boost_db`. Both channels share each band's gain.
pub struct Mbc {
    fs: f32,
    crossover_hz: [f32; 3],
    trees: [Tree; 2],
    pub bands: [BandSettings; BANDS],
    power: [f32; BANDS],
    /// Smoothed band gain in dB.
    gain_db: [f32; BANDS],
    /// Linear gain reached at the end of the last block.
    gain: [f32; BANDS],
}

impl Mbc {
    pub fn new(fs: f32) -> Self {
        let hz = [500.0, 2000.0, 8000.0];
        let flat = BandSettings { threshold_db: 0.0, slope: 0.0, boost_db: 0.0, attack_ms: 10.0, release_ms: 200.0 };
        Self {
            fs,
            crossover_hz: hz,
            trees: [Tree::new(fs, hz); 2],
            bands: [flat; BANDS],
            power: [0.0; BANDS],
            gain_db: [0.0; BANDS],
            gain: [1.0; BANDS],
        }
    }

    /// Crossovers in Hz, ascending; a change rebuilds the filters.
    pub fn set_crossovers(&mut self, hz: [f32; 3]) {
        let nyquist = 0.45 * self.fs;
        let hz = hz.map(|f| f.clamp(20.0, nyquist));
        #[allow(clippy::float_cmp)]
        let changed = hz != self.crossover_hz;
        if changed {
            self.crossover_hz = hz;
            self.trees = [Tree::new(self.fs, hz); 2];
        }
    }

    #[allow(clippy::cast_precision_loss)]
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let rms = coef(RMS_MS, self.fs);
        let rate = self.fs / STEP as f32;
        let att: [f32; BANDS] = self.bands.map(|b| coef(b.attack_ms, rate));
        let rel: [f32; BANDS] = self.bands.map(|b| coef(b.release_ms, rate));
        for (cl, cr) in l.chunks_mut(STEP).zip(r.chunks_mut(STEP)) {
            let mut split = [[[0.0f32; BANDS]; STEP]; 2];
            for (k, (xl, xr)) in cl.iter().zip(cr.iter()).enumerate() {
                split[0][k] = self.trees[0].run(*xl);
                split[1][k] = self.trees[1].run(*xr);
                for ((p, yl), yr) in self.power.iter_mut().zip(split[0][k]).zip(split[1][k]) {
                    let e = (yl * yl).max(yr * yr);
                    *p = e + (*p - e) * rms;
                }
            }
            let target: [f32; BANDS] = std::array::from_fn(|b| {
                let s = self.bands[b];
                let level = 10.0 * (self.power[b] + 1e-20).log10() + 3.0103;
                let db = if level > s.threshold_db {
                    -s.slope * (level - s.threshold_db)
                } else if level > GATE_DB {
                    (s.slope * (s.threshold_db - level)).min(s.boost_db)
                } else {
                    0.0
                };
                let a = if db < self.gain_db[b] { att[b] } else { rel[b] };
                self.gain_db[b] = db + (self.gain_db[b] - db) * a;
                db_to_lin(self.gain_db[b])
            });
            let n = cl.len() as f32;
            let step: [f32; BANDS] = std::array::from_fn(|b| (target[b] - self.gain[b]) / n);
            for (k, (xl, xr)) in cl.iter_mut().zip(cr.iter_mut()).enumerate() {
                let (mut yl, mut yr) = (0.0, 0.0);
                for b in 0..BANDS {
                    let g = self.gain[b] + step[b] * (k + 1) as f32;
                    yl += g * split[0][k][b];
                    yr += g * split[1][k][b];
                }
                (*xl, *xr) = (yl, yr);
            }
            self.gain = target;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Output RMS in dB over the last 0.25 s of a 1 s tone.
    #[allow(clippy::cast_precision_loss)]
    fn rms_out_db(m: &mut Mbc, amp: f32, hz: f32) -> f32 {
        let x: Vec<f32> = (0..48_000).map(|i| amp * (std::f32::consts::TAU * hz * i as f32 / 48_000.0).sin()).collect();
        let (mut l, mut r) = (x.clone(), x);
        for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
            m.process(cl, cr);
        }
        let tail = &l[36_000..];
        10.0 * (tail.iter().map(|s| s * s).sum::<f32>() / tail.len() as f32).log10()
    }

    #[test]
    fn flat_settings_keep_every_frequency_level() {
        for hz in [60.0, 500.0, 1200.0, 2000.0, 5000.0, 8000.0, 14_000.0] {
            let input = 20.0 * 0.3f32.log10() - 3.0103;
            let out = rms_out_db(&mut Mbc::new(48_000.0), 0.3, hz);
            assert!((out - input).abs() < 0.2, "{hz} Hz changed by {} dB", out - input);
        }
    }

    #[test]
    fn loud_band_is_compressed_and_quiet_band_lifted() {
        let mut m = Mbc::new(48_000.0);
        m.bands[0] =
            BandSettings { threshold_db: -20.0, slope: 0.5, boost_db: 4.0, attack_ms: 10.0, release_ms: 200.0 };
        let loud = 20.0 * 0.5f32.log10();
        let cut = loud - 3.0103 - rms_out_db(&mut m, 0.5, 100.0);
        assert!((cut - 7.0).abs() < 1.0, "cut {cut} dB, want 7");
        let mut m2 = Mbc::new(48_000.0);
        m2.bands[0] = m.bands[0];
        let quiet = 20.0 * 0.01f32.log10();
        let lift = rms_out_db(&mut m2, 0.01, 100.0) - (quiet - 3.0103);
        assert!((lift - 4.0).abs() < 0.5, "lift {lift} dB, want 4");
    }
}
