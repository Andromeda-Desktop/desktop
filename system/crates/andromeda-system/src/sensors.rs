//! `/org/andromeda/System1/Sensors`: the ambient light sensor (kernel iio ALS, e.g. `hid_sensor_als`), read on
//! request: the caller sets the pace, so nothing polls while no one asks. Human presence (HID usage 0x04B1,
//! fed by Wi-Fi sensing through the patched iwlmld `wifi_presence`): sensing runs and is read each
//! second only while a client holds a claim.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;
use std::time::Duration;

use anyhow::{Context, Result, anyhow};
use tokio::sync::watch;
use zbus::fdo::Error;
use zbus::message::Header;
use zbus::{Connection, interface};

use crate::polkit;
use andromeda_rt::logged::Logged;

pub const PATH: &str = "/org/andromeda/System1/Sensors";
const PRESENCE_USAGE: &str = "2004b1";

/// Whether any client claims presence; `run` turns sensing on and off from it.
static CLAIMED: LazyLock<watch::Sender<bool>> = LazyLock::new(|| watch::Sender::new(false));

/// `Presence` values.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u32)]
pub enum Presence {
    #[default]
    Unknown = 0,
    Absent = 1,
    Present = 2,
}

pub struct Sensors {
    root: PathBuf,
    /// Unique bus names that claim presence.
    claims: BTreeSet<String>,
    presence: Presence,
}

impl Default for Sensors {
    fn default() -> Self {
        Self { root: PathBuf::from("/"), claims: BTreeSet::new(), presence: Presence::Unknown }
    }
}

impl Sensors {
    fn publish_claims(&self) {
        let claimed = !self.claims.is_empty();
        CLAIMED.send_if_modified(|current| std::mem::replace(current, claimed) != claimed);
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

    /// True when `ReadLight` has a sensor to read.
    #[zbus(property)]
    fn has_light(&self) -> bool {
        light_device(&self.root).is_ok()
    }

    /// True when `ClaimPresence` has a sensor to run.
    #[zbus(property)]
    fn has_presence(&self) -> bool {
        presence_sensor(&self.root).is_ok()
    }

    /// 0 unknown (no claim, or no sensor), 1 absent, 2 present.
    #[zbus(property)]
    fn presence(&self) -> u32 {
        self.presence as u32
    }


    /// Runs presence sensing until `ReleasePresence` or until the caller leaves the bus.
    #[allow(clippy::needless_pass_by_value)] // zbus hands the header by value
    async fn claim_presence(
        &mut self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] conn: &Connection,
    ) -> zbus::fdo::Result<()> {
        polkit::check(conn, &header, "org.andromeda.system.presence").await?;
        presence_sensor(&self.root).map_err(|e| Error::NotSupported(format!("{e:#}")))?;
        let owner = header.sender().ok_or_else(|| Error::Failed("no sender".into()))?.to_string();
        if !self.claims.insert(owner.clone()) {
            return Ok(());
        }
        self.publish_claims();
        let conn = conn.clone();
        tokio::spawn(async move {
            if crate::bus::vanished(&conn, &owner).await.is_ok()
                && let Ok(iface) = conn.object_server().interface::<_, Sensors>(PATH).await
            {
                let mut sensors = iface.get_mut().await;
                sensors.claims.remove(&owner);
                sensors.publish_claims();
            }
        });
        Ok(())
    }

    #[allow(clippy::needless_pass_by_value)] // zbus hands the header by value
    fn release_presence(&mut self, #[zbus(header)] header: Header<'_>) {
        if let Some(owner) = header.sender() {
            self.claims.remove(owner.as_str());
            self.publish_claims();
        }
    }
}

/// Turns presence sensing on while some client claims it, and reads it each second; no timer runs otherwise.
pub async fn run(conn: Connection) {
    let mut claimed = CLAIMED.subscribe();
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    let mut sensing = false;
    loop {
        let want = *claimed.borrow_and_update();
        if want != sensing {
            sensing = want;
            match tokio::task::spawn_blocking(move || set_sensing(Path::new("/"), want)).await {
                Ok(Ok(())) => {}
                Ok(Err(e)) => log::warn!("sensors: presence sensing {}: {e:#}", if want { "on" } else { "off" }),
                Err(e) => log::warn!("sensors: {e}"),
            }
        }
        let reading = if sensing {
            tokio::task::spawn_blocking(|| read_presence(Path::new("/")))
                .await
                .logged("presence read task")
                .and_then(Result::ok)
        } else {
            None
        };
        if let Err(e) = publish(&conn, reading.unwrap_or_default()).await {
            log::warn!("sensors: {e}");
        }
        tokio::select! {
            _ = tick.tick(), if sensing => {}
            r = claimed.changed() => if r.is_err() { return },
        }
    }
}

async fn publish(conn: &Connection, presence: Presence) -> zbus::Result<()> {
    let iface = conn.object_server().interface::<_, Sensors>(PATH).await?;
    let mut sensors = iface.get_mut().await;
    if sensors.presence != presence {
        sensors.presence = presence;
        sensors.presence_changed(iface.signal_emitter()).await?;
    }
    Ok(())
}

/// The input field directory of `usage` in a HID custom sensor, e.g. `input-7-2004b1`.
fn field(sensor: &Path, usage: &str) -> Option<PathBuf> {
    std::fs::read_dir(sensor)
        .ok()?
        .flatten()
        .map(|e| e.path())
        .find(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("input-") && n.ends_with(usage)))
}

fn presence_sensor(root: &Path) -> Result<PathBuf> {
    let devices = root.join("sys/bus/platform/devices");
    std::fs::read_dir(&devices)
        .with_context(|| devices.display().to_string())?
        .flatten()
        .map(|e| e.path())
        .find(|p| field(p, PRESENCE_USAGE).is_some())
        .ok_or_else(|| anyhow!("no presence sensor"))
}

fn field_value(field: &Path) -> Result<f64> {
    let name = field.file_name().and_then(|n| n.to_str()).unwrap_or_default();
    read_f64(&field.join(format!("{name}-value")))
}

/// The Wi-Fi firmware feeds the sensor only while `wifi_presence` is on; the sensor reports only while enabled.
fn set_sensing(root: &Path, on: bool) -> Result<()> {
    let value = if on { "1" } else { "0" };
    let sensor = presence_sensor(root)?;
    std::fs::write(sensor.join("enable_sensor"), value).context("enable_sensor")?;
    let drivers = root.join("sys/bus/pci/drivers/iwlwifi");
    for device in std::fs::read_dir(&drivers).into_iter().flatten().flatten() {
        let switch = device.path().join("wifi_presence");
        if switch.exists() {
            std::fs::write(&switch, value).with_context(|| switch.display().to_string())?;
        }
    }
    Ok(())
}

/// One read per call: each field read is a request to the ISH, which wakes it and the CPU.
fn read_presence(root: &Path) -> Result<Presence> {
    let sensor = presence_sensor(root)?;
    let present = field(&sensor, PRESENCE_USAGE).ok_or_else(|| anyhow!("no presence field"))?;
    Ok(if field_value(&present)? > 0.0 { Presence::Present } else { Presence::Absent })
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
    use super::{Presence, read_light, read_presence, set_sensing};
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

    #[test]
    fn presence_sensing_follows_the_switch_and_reads_the_fields() {
        let root = std::env::temp_dir().join(format!("sensors-presence-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        assert!(read_presence(&root).is_err());
        let sensor = root.join("sys/bus/platform/devices/HID-SENSOR-2000e1.6.auto");
        let wifi = root.join("sys/bus/pci/drivers/iwlwifi/0000:00:14.3");
        for (dir, file, value) in
            [("input-7-2004b1", "input-7-2004b1-value", "1\n"), ("input-6-2004b2", "input-6-2004b2-value", "1200\n")]
        {
            fs::create_dir_all(sensor.join(dir)).unwrap();
            fs::write(sensor.join(dir).join(file), value).unwrap();
        }
        fs::write(sensor.join("enable_sensor"), "0\n").unwrap();
        fs::create_dir_all(&wifi).unwrap();
        fs::write(wifi.join("wifi_presence"), "0\n").unwrap();

        set_sensing(&root, true).unwrap();
        assert_eq!(fs::read_to_string(sensor.join("enable_sensor")).unwrap(), "1");
        assert_eq!(fs::read_to_string(wifi.join("wifi_presence")).unwrap(), "1");
        assert_eq!(read_presence(&root).unwrap(), Presence::Present);
        fs::write(sensor.join("input-7-2004b1/input-7-2004b1-value"), "0\n").unwrap();
        assert_eq!(read_presence(&root).unwrap(), Presence::Absent);
        set_sensing(&root, false).unwrap();
        assert_eq!(fs::read_to_string(sensor.join("enable_sensor")).unwrap(), "0");
        assert_eq!(fs::read_to_string(wifi.join("wifi_presence")).unwrap(), "0");
        fs::remove_dir_all(&root).unwrap();
    }
}
