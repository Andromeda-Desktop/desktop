//! The system part's policy state (docs/system-health.md §5-§6): the knob table and rule engine, the explain log,
//! and the actuators that apply a knob's effective value to its backend.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Result, anyhow};
use tokio::sync::Mutex;
use zbus::Connection;
use zbus::object_server::SignalEmitter;

use crate::engine::{Changes, Engine};
use crate::explain::{self, Log};
use crate::holds::{Knob, KnobValue, Merge, Source, UserChoice};
use crate::{battery, power, wifi};

pub const PROFILE: &str = "power.profile";
/// On: each battery that has a charge limit stops at its end value.
pub const CHARGE_LIMIT: &str = "battery.charge_limit";
pub const WIFI_POWER_SAVE: &str = "network.wifi_power_save";

pub struct Policy {
    pub engine: Engine,
    pub log: Log,
    /// The user's choices of `Kept` knobs, stored across restarts (TOML: knob name = value).
    choices: PathBuf,
    /// Our power-profiles-daemon hold: cookie and profile.
    ppd_hold: Option<(u32, String)>,
    /// Wi-Fi power save before the first hold turned it on; restored when the last hold ends.
    wifi_before: Option<bool>,
}

pub type Shared = Arc<Mutex<Policy>>;

pub fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
}

pub fn new(log: PathBuf, choices: PathBuf) -> Shared {
    let knobs = vec![
        Knob::new(PROFILE, Merge::LowestPower),
        Knob::kept(CHARGE_LIMIT, Merge::AnyTrue),
        Knob::new(WIFI_POWER_SAVE, Merge::AnyTrue),
    ];
    let (mut engine, _) = Engine::new(knobs, Vec::new(), now_ms());
    load_choices(&choices, &mut engine);
    Arc::new(Mutex::new(Policy { engine, log: Log::new(log), choices, ppd_hold: None, wifi_before: None }))
}

/// Restores the stored user choices; an unreadable file or entry is skipped.
pub fn load_choices(path: &Path, engine: &mut Engine) {
    let table = match std::fs::read_to_string(path) {
        Ok(text) => match text.parse::<toml::Table>() {
            Ok(table) => table,
            Err(e) => return log::warn!("{}: {e}", path.display()),
        },
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return,
        Err(e) => return log::warn!("{}: {e}", path.display()),
    };
    for (name, value) in &table {
        let value = engine
            .knob(name)
            .filter(|k| k.choice == UserChoice::Kept)
            .and_then(|k| KnobValue::from_toml(value, k.merge));
        match value {
            Some(value) => _ = engine.set_baseline(name, value, now_ms()),
            None => log::warn!("{}: ignored {name}", path.display()),
        }
    }
}

/// Writes every `Kept` user choice (temp file, then rename).
pub fn save_choices(path: &Path, engine: &Engine) -> std::io::Result<()> {
    let table: toml::Table =
        engine.knobs().filter_map(|k| Some((k.name.to_owned(), k.kept_choice()?.to_toml()))).collect();
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, table.to_string())?;
    std::fs::rename(tmp, path)
}

impl Policy {
    /// Stores the `Kept` user choices; a failed write keeps them for this run only.
    pub fn store_choices(&self) {
        if let Err(e) = save_choices(&self.choices, &self.engine) {
            log::warn!("{}: {e}", self.choices.display());
        }
    }

    /// Writes the entries, then applies each moved knob. `user_profile` is set when the user picked a profile: it
    /// becomes `ActiveProfile` first (power-profiles-daemon drops every hold then), and our hold is placed again.
    pub async fn commit(&mut self, conn: &Connection, changes: Changes, user_profile: Option<&str>) -> Result<()> {
        for entry in &changes.entries {
            if let Err(e) = self.log.append(entry) {
                log::warn!("explain log: {e}");
            }
            let emitter = SignalEmitter::new(conn, explain::PATH)?;
            explain::Explain::entry(&emitter, entry.to_dict()).await?;
        }
        if let Some(profile) = user_profile {
            power::set_profile(conn, profile).await?;
            self.ppd_hold = None;
        }
        if user_profile.is_some() || changes.apply.iter().any(|(k, _)| *k == PROFILE) {
            self.apply_profile(conn).await?;
        }
        if let Some((_, Some(KnobValue::Bool(on)))) = changes.apply.iter().find(|(k, _)| *k == CHARGE_LIMIT) {
            battery::set_charge_limit(conn, *on).await?;
        }
        if let Some((_, value)) = changes.apply.iter().find(|(k, _)| *k == WIFI_POWER_SAVE) {
            self.apply_wifi_power_save(value.as_ref())?;
        }
        Ok(())
    }

    fn apply_wifi_power_save(&mut self, value: Option<&KnobValue>) -> Result<()> {
        if value == Some(&KnobValue::Bool(true)) {
            if self.wifi_before.is_none() {
                self.wifi_before = wifi::power_save()?;
            }
            return wifi::set_power_save(true);
        }
        match self.wifi_before.take() {
            Some(before) => wifi::set_power_save(before),
            None => Ok(()),
        }
    }

    async fn apply_profile(&mut self, conn: &Connection) -> Result<()> {
        let Some(knob) = self.engine.knob(PROFILE) else { return Ok(()) };
        let (value, source) = knob.effective();
        let wanted = match (value, source) {
            (Some(KnobValue::Str(p)), Source::Holds(..)) if Some(&p) != knob_baseline(knob).as_ref() => Some(p),
            _ => None,
        };
        if self.ppd_hold.as_ref().map(|(_, p)| p) == wanted.as_ref() {
            return Ok(());
        }
        if let Some((cookie, _)) = self.ppd_hold.take() {
            power::release_profile(conn, cookie).await?;
        }
        if let Some(profile) = wanted {
            if profile == "balanced" {
                return Err(anyhow!("power-profiles-daemon cannot hold balanced"));
            }
            let cookie = power::hold_profile(conn, &profile, "andromeda-system rule").await?;
            self.ppd_hold = Some((cookie, profile));
        }
        Ok(())
    }
}

fn knob_baseline(knob: &Knob) -> Option<String> {
    match knob.baseline.value() {
        Some(KnobValue::Str(p)) => Some(p.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::{load_choices, save_choices};
    use crate::engine::Engine;
    use crate::holds::{Baseline, Knob, KnobValue, Merge};

    #[derive(Clone, Copy, Debug)]
    enum Event {
        User(bool),
        AutoHold,
        Release,
        Restart,
    }

    fn engine(path: &std::path::Path) -> Engine {
        let knobs =
            vec![Knob::kept("battery.charge_limit", Merge::AnyTrue), Knob::new("power.profile", Merge::LowestPower)];
        let mut engine = Engine::new(knobs, Vec::new(), 0).0;
        load_choices(path, &mut engine);
        engine
    }

    /// Every sequence of user choices, automatic holds (as `battery.optimized_charging` places them), releases and
    /// restarts: once the user chose, the charge limit is that choice in every later state; before it, an automatic
    /// hold turns it on. `power.profile` keeps the episode model: its user choice is not stored.
    #[test]
    fn kept_user_choice_beats_automatic_holds_across_restarts() {
        let events = [Event::User(true), Event::User(false), Event::AutoHold, Event::Release, Event::Restart];
        let dir = std::env::temp_dir().join(format!("andromeda-kept-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("knobs.toml");
        let len = 5;
        for n in 0..events.len().pow(len) {
            let _ = std::fs::remove_file(&path);
            let mut e = engine(&path);
            let (mut user, mut seeded) = (None, false);
            let mut held;
            let mut trace = Vec::new();
            let mut code = n;
            for _ in 0..len {
                let event = events[code % events.len()];
                code /= events.len();
                trace.push(event);
                match event {
                    Event::User(on) => {
                        e.set_baseline("battery.charge_limit", KnobValue::Bool(on), 0).unwrap();
                        e.set_baseline("power.profile", KnobValue::Str("performance".into()), 0).unwrap();
                        save_choices(&path, &e).unwrap();
                        (user, held) = (Some(on), false);
                    }
                    Event::AutoHold => {
                        if e.knob("battery.charge_limit").unwrap().baseline == Baseline::Unknown {
                            e.init_baseline("battery.charge_limit", KnobValue::Bool(true));
                            seeded = true;
                        }
                        e.hold("battery.charge_limit", ":1.9", "battery.optimized_charging", &true.into(), 0).unwrap();
                        e.hold("power.profile", ":1.9", "power.low_power_plan", &"power-saver".into(), 0).unwrap();
                        held = true;
                    }
                    Event::Release => {
                        e.release(":1.9", None);
                        held = false;
                    }
                    Event::Restart => {
                        e = engine(&path);
                        (held, seeded) = (false, false);
                    }
                }
                let expected = match (user, held) {
                    (Some(on), _) => Some(KnobValue::Bool(on)),
                    (None, true) => Some(KnobValue::Bool(true)),
                    (None, false) => seeded.then_some(KnobValue::Bool(true)),
                };
                assert_eq!(e.knob("battery.charge_limit").unwrap().effective().0, expected, "{trace:?}");
                let profile = e.knob("power.profile").unwrap();
                if held {
                    assert_eq!(profile.effective().0, Some(KnobValue::Str("power-saver".into())), "{trace:?}");
                }
                if matches!(event, Event::Restart) {
                    assert_eq!(profile.baseline, Baseline::Unknown, "{trace:?}");
                }
            }
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
