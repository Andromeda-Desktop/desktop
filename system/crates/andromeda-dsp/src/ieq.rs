use crate::biquad::{Biquad, Coeffs};
use crate::coef;
use crate::regulator::{BAND_HZ, BANDS, band_q};

/// Gains are recomputed once per this many samples.
const STEP: usize = 32;
/// Below this mean band level (dB) the signal is silence or a fade: gains hold.
const GATE_DB: f32 = -70.0;
/// A band this far below the mean has no content to shape (a low-passed file): it gets no boost.
const EMPTY_DB: f32 = 30.0;

/// Adaptive EQ that moves the programme's spectral balance toward a target curve.
///
/// Each band measures its long-term level; the band gain is `amount` times the distance between the
/// target and the measured balance, both taken relative to their mean over the bands, within ±`max_db`.
/// Both channels share one measurement and one set of gains, so the stereo image does not move.
pub struct Ieq {
    fs: f32,
    bands: Vec<Band>,
    /// Inverse of the bells' interaction: bell gains that put each wanted gain at its own band centre.
    unmix: Vec<Vec<f32>>,
    /// Target band level in dB; only the shape matters.
    pub target_db: [f32; BANDS],
    /// 0 = off, 1 = full correction.
    pub amount: f32,
    pub max_db: f32,
    /// Level averaging time constant.
    pub detect_ms: f32,
    /// Gain glide time constant.
    pub glide_ms: f32,
}

struct Band {
    hz: f32,
    q: f32,
    /// Runs on the mid (L+R)/2 signal.
    detect: Biquad,
    power: f32,
    eq: [Biquad; 2],
    gain_db: f32,
    /// Gain the `eq` filters were last set to.
    applied_db: f32,
}

impl Ieq {
    pub fn new(fs: f32) -> Self {
        let bands = (0..BANDS)
            .filter(|&i| BAND_HZ[i] < 0.45 * fs)
            .map(|i| {
                let (hz, q) = (BAND_HZ[i], band_q(i));
                Band {
                    hz,
                    q,
                    detect: Biquad::new(Coeffs::bandpass(fs, hz, q)),
                    power: 0.0,
                    eq: [Biquad::new(Coeffs::peaking(fs, hz, q, 0.0)); 2],
                    gain_db: 0.0,
                    applied_db: 0.0,
                }
            })
            .collect::<Vec<Band>>();
        let interaction = bands
            .iter()
            .map(|at| bands.iter().map(|b| Coeffs::peaking(fs, b.hz, b.q, 1.0).db_at(fs, at.hz)).collect())
            .collect();
        Self {
            fs,
            unmix: invert(interaction),
            bands,
            target_db: [0.0; BANDS],
            amount: 0.0,
            max_db: 6.0,
            detect_ms: 1000.0,
            glide_ms: 500.0,
        }
    }

    #[allow(clippy::cast_precision_loss, clippy::many_single_char_names)]
    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let det = coef(self.detect_ms, self.fs);
        let glide = coef(self.glide_ms, self.fs / STEP as f32);
        let n = self.bands.len() as f32;
        let target_mean = self.target_db[..self.bands.len()].iter().sum::<f32>() / n;
        let mut levels = [0.0f32; BANDS];
        for (cl, cr) in l.chunks_mut(STEP).zip(r.chunks_mut(STEP)) {
            for (band, level) in self.bands.iter_mut().zip(&mut levels) {
                for (&xl, &xr) in cl.iter().zip(cr.iter()) {
                    let y = band.detect.run(f32::midpoint(xl, xr));
                    let e = y * y;
                    band.power = (e + (band.power - e) * det + 1e-30) - 1e-30;
                }
                *level = 10.0 * (band.power + 1e-20).log10();
            }
            let mean = levels[..self.bands.len()].iter().sum::<f32>() / n;
            let mut want = [0.0f32; BANDS];
            for (i, w) in want.iter_mut().enumerate().take(self.bands.len()) {
                if levels[i] >= mean - EMPTY_DB {
                    *w = (self.amount * ((self.target_db[i] - target_mean) - (levels[i] - mean)))
                        .clamp(-self.max_db, self.max_db);
                }
            }
            for (band, row) in self.bands.iter_mut().zip(&self.unmix) {
                let want = if mean < GATE_DB {
                    band.gain_db
                } else {
                    row.iter().zip(&want).map(|(m, w)| m * w).sum::<f32>().clamp(-2.0 * self.max_db, 2.0 * self.max_db)
                };
                band.gain_db = want + (band.gain_db - want) * glide;
                if (band.gain_db - band.applied_db).abs() > 0.01 {
                    band.applied_db = band.gain_db;
                    let c = Coeffs::peaking(self.fs, band.hz, band.q, band.gain_db);
                    band.eq.iter_mut().for_each(|f| f.set(c));
                }
                for (f, block) in band.eq.iter_mut().zip([&mut *cl, &mut *cr]) {
                    for x in block.iter_mut() {
                        *x = f.run(*x);
                    }
                }
            }
        }
    }
}

/// Inverse of a square matrix by Gauss-Jordan elimination with partial pivoting.
fn invert(mut a: Vec<Vec<f32>>) -> Vec<Vec<f32>> {
    let n = a.len();
    let mut inv: Vec<Vec<f32>> = (0..n).map(|i| (0..n).map(|j| if i == j { 1.0 } else { 0.0 }).collect()).collect();
    for c in 0..n {
        let piv = (c..n).max_by(|&x, &y| a[x][c].abs().total_cmp(&a[y][c].abs())).unwrap_or(c);
        a.swap(c, piv);
        inv.swap(c, piv);
        let d = a[c][c];
        a[c].iter_mut().for_each(|v| *v /= d);
        inv[c].iter_mut().for_each(|v| *v /= d);
        for r in (0..n).filter(|&r| r != c) {
            let f = a[r][c];
            for k in 0..n {
                a[r][k] -= f * a[c][k];
                inv[r][k] -= f * inv[c][k];
            }
        }
    }
    inv
}

#[cfg(test)]
mod tests {
    use super::*;

    /// UX3405CA `ieq_detailed`, converted from 1/16 dB.
    fn ux3405_target() -> [f32; BANDS] {
        [
            150., 142., 188., 216., 189., 195., 202., 199., 210., 225., 230., 236., 235., 235., 214., 165., 112., 49.,
            -24., -217.,
        ]
        .map(|v| v / 16.0)
    }

    /// Pink noise (equal energy per octave, as music on average), Paul Kellet's filter.
    fn pink(len: usize) -> Vec<f32> {
        let mut s = 0x1234_5678u32;
        let mut b = [0.0f32; 3];
        (0..len)
            .map(|_| {
                s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                #[allow(clippy::cast_precision_loss)]
                let w = (s >> 8) as f32 / (1u32 << 24) as f32 - 0.5;
                b[0] = 0.99765 * b[0] + w * 0.099_046;
                b[1] = 0.96300 * b[1] + w * 0.296_516_4;
                b[2] = 0.57000 * b[2] + w * 1.052_691_3;
                0.05 * (b[0] + b[1] + b[2] + w * 0.1848)
            })
            .collect()
    }

    /// RMS distance in dB between the band balance of `x` and the target balance, over bands 1..=17.
    #[allow(clippy::cast_precision_loss)]
    fn balance_error(x: &[f32], target: &[f32; BANDS]) -> f32 {
        let bands = 1..18;
        let levels: Vec<f32> = bands
            .clone()
            .map(|i| {
                let mut f = Biquad::new(Coeffs::bandpass(48_000.0, BAND_HZ[i], band_q(i)));
                let p = x.iter().map(|&s| f.run(s)).map(|y| y * y).sum::<f32>() / x.len() as f32;
                10.0 * p.log10()
            })
            .collect();
        let n = levels.len() as f32;
        let (lm, tm) = (levels.iter().sum::<f32>() / n, target[bands.clone()].iter().sum::<f32>() / n);
        let sq: f32 = levels.iter().zip(&target[bands]).map(|(l, t)| ((l - lm) - (t - tm)).powi(2)).sum();
        (sq / n).sqrt()
    }

    #[test]
    fn pink_noise_moves_toward_target_balance() {
        let target = ux3405_target();
        let x = pink(4 * 48_000);
        let mut ieq = Ieq::new(48_000.0);
        ieq.target_db = target;
        ieq.amount = 1.0;
        ieq.max_db = 12.0;
        let (mut l, mut r) = (x.clone(), x.clone());
        for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
            ieq.process(cl, cr);
        }
        let tail = 3 * 48_000;
        let (before, after) = (balance_error(&x[tail..], &target), balance_error(&l[tail..], &target));
        assert!(after < 0.5 * before, "balance error {before:.2} dB -> {after:.2} dB");
    }
}
