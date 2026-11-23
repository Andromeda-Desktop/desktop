//! `/org/andromeda/System1/Sensors`: the ambient light sensor (kernel iio ALS, e.g. `hid_sensor_als`), read on
//! request: the caller sets the pace, so nothing polls while no one asks.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result, anyhow};
use zbus::fdo::Error;
use zbus::interface;

pub const PATH: &str = "/org/andromeda/System1/Sensors";

pub struct Sensors {
    root: PathBuf,
}

impl Default for Sensors {
    fn default() -> Self {
        Self { root: PathBuf::from("/") }
    }
}

#[interface(name = "org.andromeda.System1.Sensors")]
impl Sensors {
    /// Illuminance in lux and correlated colour temperature in kelvin (0 when the sensor has no colour channel).
    async fn read_light(&self) -> zbus::fdo::Result<(f64, u32)> {
        let root = self.root.clone();
        tokio::task::spawn_blocking(move || read_light(&root))
            .await
            .map_err(|e| Error::Failed(e.to_string()))?
            .map_err(|e| Error::NotSupported(format!("{e:#}")))
    }
}

fn read_f64(path: &Path) -> Result<f64> {
    let text = std::fs::read_to_string(path).with_context(|| path.display().to_string())?;
    text.trim().parse().with_context(|| format!("{}: {text:?}", path.display()))
}

/// `(raw + offset) * scale` of one iio channel; offset and scale are optional.
fn channel(device: &Path, name: &str) -> Result<f64> {
    let raw = read_f64(&device.join(format!("in_{name}_raw")))?;
    let offset = read_f64(&device.join(format!("in_{name}_offset"))).unwrap_or(0.0);
    let scale = read_f64(&device.join(format!("in_{name}_scale"))).unwrap_or(1.0);
    Ok((raw + offset) * scale)
}

fn light_device(root: &Path) -> Result<PathBuf> {
    let devices = root.join("sys/bus/iio/devices");
    for entry in std::fs::read_dir(&devices).with_context(|| devices.display().to_string())?.flatten() {
        if entry.path().join("in_illuminance_raw").exists() {
            return Ok(entry.path());
        }
    }
    Err(anyhow!("no ambient light sensor"))
}

fn read_light(root: &Path) -> Result<(f64, u32)> {
    let device = light_device(root)?;
    let lux = channel(&device, "illuminance")?.max(0.0);
    #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss, reason = "clamped to 0..=u32::MAX kelvin")]
    let kelvin = channel(&device, "colortemp").map_or(0, |k| k.clamp(0.0, f64::from(u32::MAX)).round() as u32);
    Ok((lux, kelvin))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::read_light;
    use std::fs;
    use std::path::Path;

    fn sensor(root: &Path, device: &str, files: &[(&str, &str)]) {
        let dir = root.join("sys/bus/iio/devices").join(device);
        fs::create_dir_all(&dir).unwrap();
        for (name, value) in files {
            fs::write(dir.join(name), value).unwrap();
        }
    }

    #[test]
    fn reads_lux_and_kelvin_with_scale_and_offset() {
        let root = std::env::temp_dir().join(format!("sensors-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        sensor(&root, "iio:device1", &[("name", "accel_3d\n"), ("in_accel_x_raw", "5\n")]);
        assert!(read_light(&root).is_err());
        sensor(
            &root,
            "iio:device0",
            &[
                ("in_illuminance_raw", "83914\n"),
                ("in_illuminance_scale", "0.001000000\n"),
                ("in_illuminance_offset", "0\n"),
                ("in_colortemp_raw", "2995672\n"),
                ("in_colortemp_scale", "0.001000000\n"),
            ],
        );
        let (lux, kelvin) = read_light(&root).unwrap();
        assert!((lux - 83.914).abs() < 1e-9);
        assert_eq!(kelvin, 2996);
        fs::remove_file(root.join("sys/bus/iio/devices/iio:device0/in_colortemp_raw")).unwrap();
        assert_eq!(read_light(&root).unwrap().1, 0);
        fs::remove_dir_all(&root).unwrap();
    }
}
