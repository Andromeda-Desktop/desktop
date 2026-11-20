use crate::biquad::{Biquad, Coeffs};

/// Stereo widener: adds the high-passed side (L-R) signal back, so the image widens while the
/// bass, which the small speakers play best in phase, stays centred.
pub struct Widen {
    fs: f32,
    /// Extra side gain, 0 = off, 1 = side doubled.
    pub width: f32,
    pub cutoff_hz: f32,
    hp: Biquad,
    built_hz: f32,
}

impl Widen {
    pub fn new(fs: f32) -> Self {
        let cutoff_hz = 300.0;
        Self {
            fs,
            width: 0.0,
            cutoff_hz,
            hp: Biquad::new(Coeffs::highpass(fs, cutoff_hz, std::f32::consts::FRAC_1_SQRT_2)),
            built_hz: cutoff_hz,
        }
    }

    pub fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        #[allow(clippy::float_cmp)]
        let changed = self.cutoff_hz != self.built_hz;
        if changed {
            self.built_hz = self.cutoff_hz;
            let hz = self.cutoff_hz.clamp(20.0, 0.45 * self.fs);
            self.hp.set(Coeffs::highpass(self.fs, hz, std::f32::consts::FRAC_1_SQRT_2));
        }
        for (xl, xr) in l.iter_mut().zip(r.iter_mut()) {
            let side = 0.5 * (*xl - *xr);
            let extra = self.width * self.hp.run(side);
            *xl += extra;
            *xr -= extra;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[allow(clippy::cast_precision_loss)]
    fn tone(hz: f32, amp: f32) -> Vec<f32> {
        (0..48_000).map(|i| amp * (std::f32::consts::TAU * hz * i as f32 / 48_000.0).sin()).collect()
    }

    fn rms(x: &[f32]) -> f32 {
        #[allow(clippy::cast_precision_loss)]
        let n = x.len() as f32;
        (x.iter().map(|s| s * s).sum::<f32>() / n).sqrt()
    }

    #[test]
    fn side_above_cutoff_grows_while_mid_and_bass_side_stay() {
        let mut w = Widen::new(48_000.0);
        w.width = 1.0;
        let (mut l, mut r) = (tone(2000.0, 0.2), tone(2000.0, -0.2));
        w.process(&mut l, &mut r);
        let gain = rms(&l[24_000..]) / rms(&tone(2000.0, 0.2)[24_000..]);
        assert!((gain - 2.0).abs() < 0.1, "side gain {gain}");

        let mut w = Widen::new(48_000.0);
        w.width = 1.0;
        let (mut l, mut r) = (tone(2000.0, 0.2), tone(2000.0, 0.2));
        w.process(&mut l, &mut r);
        assert!((rms(&l[24_000..]) - rms(&tone(2000.0, 0.2)[24_000..])).abs() < 1e-3, "mid changed");

        let mut w = Widen::new(48_000.0);
        w.width = 1.0;
        let (mut l, mut r) = (tone(40.0, 0.2), tone(40.0, -0.2));
        w.process(&mut l, &mut r);
        let bass = rms(&l[24_000..]) / rms(&tone(40.0, 0.2)[24_000..]);
        assert!(bass < 1.1, "bass side gain {bass}");
    }
}
