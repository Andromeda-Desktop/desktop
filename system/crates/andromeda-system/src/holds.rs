//! Knob arbitration (docs/system-health.md §5). The user's choice is the knob's baseline. Automatic holds replace
//! it while their episode runs; a user change during such an episode is that episode's Undo. Safety holds win over
//! everything, the user's choice included. Inside one priority the knob's merge decides. A `Kept` knob's user choice
//! is a standing preference: automatic holds do not replace it, and it is stored across restarts.

use std::collections::BTreeSet;

use crate::rules::Priority;

#[derive(Clone, Debug, PartialEq)]
pub enum KnobValue {
    Str(String),
    Num(f64),
    Bool(bool),
    Set(BTreeSet<String>),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Merge {
    /// Profiles: power-saver < balanced < performance; the lowest wins (as power-profiles-daemon holds do).
    LowestPower,
    Min,
    AnyTrue,
    Union,
}

impl KnobValue {
    pub fn to_toml(&self) -> toml::Value {
        match self {
            Self::Str(s) => s.clone().into(),
            Self::Num(n) => (*n).into(),
            Self::Bool(b) => (*b).into(),
            Self::Set(items) => items.iter().cloned().collect::<Vec<_>>().into(),
        }
    }

    /// A rule's hold value, if it has the type the merge needs.
    pub fn from_toml(value: &toml::Value, merge: Merge) -> Option<Self> {
        match (merge, value) {
            (Merge::LowestPower, toml::Value::String(s)) if PROFILE_ORDER.contains(&s.as_str()) => {
                Some(Self::Str(s.clone()))
            }
            #[allow(clippy::cast_precision_loss)] // knob values are small
            (Merge::Min, toml::Value::Integer(i)) => Some(Self::Num(*i as f64)),
            (Merge::Min, toml::Value::Float(f)) => Some(Self::Num(*f)),
            (Merge::AnyTrue, toml::Value::Boolean(b)) => Some(Self::Bool(*b)),
            (Merge::Union, toml::Value::Array(items)) => {
                items.iter().map(|i| i.as_str().map(str::to_owned)).collect::<Option<_>>().map(Self::Set)
            }
            _ => None,
        }
    }
}

impl From<&KnobValue> for zbus::Variant<'static> {
    fn from(value: &KnobValue) -> Self {
        match value {
            KnobValue::Str(s) => s.clone().into(),
            KnobValue::Num(n) => (*n).into(),
            KnobValue::Bool(b) => (*b).into(),
            KnobValue::Set(items) => items.iter().cloned().collect::<Vec<_>>().into(),
        }
    }
}

impl std::fmt::Display for KnobValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Str(s) => f.write_str(s),
            Self::Num(n) => write!(f, "{n}"),
            Self::Bool(b) => write!(f, "{b}"),
            Self::Set(items) => f.write_str(&items.iter().map(String::as_str).collect::<Vec<_>>().join(",")),
        }
    }
}

const PROFILE_ORDER: [&str; 3] = ["power-saver", "balanced", "performance"];

fn merge(kind: Merge, values: impl Iterator<Item = KnobValue>) -> Option<KnobValue> {
    values.reduce(|a, b| match (kind, a, b) {
        (Merge::LowestPower, KnobValue::Str(x), KnobValue::Str(y)) => {
            let rank = |p: &str| PROFILE_ORDER.iter().position(|q| *q == p).unwrap_or(PROFILE_ORDER.len());
            KnobValue::Str(if rank(&y) < rank(&x) { y } else { x })
        }
        (Merge::Min, KnobValue::Num(x), KnobValue::Num(y)) => KnobValue::Num(x.min(y)),
        (Merge::AnyTrue, KnobValue::Bool(x), KnobValue::Bool(y)) => KnobValue::Bool(x || y),
        (Merge::Union, KnobValue::Set(mut x), KnobValue::Set(y)) => {
            x.extend(y);
            KnobValue::Set(x)
        }
        (_, a, _) => a,
    })
}

#[derive(Clone, Debug, PartialEq)]
pub struct Hold {
    pub id: u64,
    /// The rule id, or the bus name of a client that holds the knob directly.
    pub owner: String,
    pub priority: Priority,
    pub value: KnobValue,
    /// The rule id that asked for it.
    pub reason: String,
    pub since_ms: u64,
}

/// The knob's value while no hold wins.
#[derive(Clone, Debug, PartialEq)]
pub enum Baseline {
    /// Not read from the backend yet.
    Unknown,
    /// The backend's own state before the user's first choice.
    Backend(KnobValue),
    User(KnobValue),
}

impl Baseline {
    pub fn value(&self) -> Option<&KnobValue> {
        match self {
            Self::Unknown => None,
            Self::Backend(v) | Self::User(v) => Some(v),
        }
    }
}

/// What the user's choice does to automatic holds.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum UserChoice {
    /// It undoes the running automatic episode; the next episode holds the knob again.
    UndoesEpisode,
    /// A standing preference (a Settings switch): it beats automatic holds and outlives restarts.
    Kept,
}

#[derive(Debug)]
pub struct Knob {
    pub name: &'static str,
    pub merge: Merge,
    pub choice: UserChoice,
    pub baseline: Baseline,
    pub holds: Vec<Hold>,
}

/// Where the effective value comes from, for the explain log and "Limited by <rule>" in Settings.
#[derive(Clone, Debug, PartialEq)]
pub enum Source {
    Baseline,
    Holds(Priority, Vec<String>),
}

impl Knob {
    pub fn new(name: &'static str, merge: Merge) -> Self {
        Self { name, merge, choice: UserChoice::UndoesEpisode, baseline: Baseline::Unknown, holds: Vec::new() }
    }

    pub fn kept(name: &'static str, merge: Merge) -> Self {
        Self { choice: UserChoice::Kept, ..Self::new(name, merge) }
    }

    /// The user's standing choice, if the knob keeps one and the user made it.
    pub fn kept_choice(&self) -> Option<&KnobValue> {
        match (&self.baseline, self.choice) {
            (Baseline::User(v), UserChoice::Kept) => Some(v),
            _ => None,
        }
    }

    /// The value the owner applies, and why.
    pub fn effective(&self) -> (Option<KnobValue>, Source) {
        let winning = match self.holds.iter().map(|h| h.priority).max() {
            Some(Priority::Automatic) if self.kept_choice().is_some() => None,
            Some(Priority::User) => None,
            top => top,
        };
        match winning {
            Some(priority) => {
                let winners: Vec<&Hold> = self.holds.iter().filter(|h| h.priority == priority).collect();
                let value = merge(self.merge, winners.iter().map(|h| h.value.clone()));
                (value, Source::Holds(priority, winners.iter().map(|h| h.owner.clone()).collect()))
            }
            _ => (self.baseline.value().cloned(), Source::Baseline),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Baseline, Hold, Knob, KnobValue, Merge, Source};
    use crate::rules::Priority;

    fn profile(p: &str) -> KnobValue {
        KnobValue::Str(p.to_owned())
    }

    fn hold(id: u64, owner: &str, priority: Priority, value: KnobValue) -> Hold {
        Hold { id, owner: owner.to_owned(), priority, value, reason: owner.to_owned(), since_ms: 0 }
    }

    #[test]
    fn baseline_until_a_hold_then_safety_beats_automatic() {
        let mut knob = Knob::new("power.profile", Merge::LowestPower);
        knob.baseline = Baseline::User(profile("performance"));
        assert_eq!(knob.effective(), (Some(profile("performance")), Source::Baseline));

        knob.holds.push(hold(1, "power.low_power_plan", Priority::Automatic, profile("power-saver")));
        assert_eq!(knob.effective().0, Some(profile("power-saver")));

        knob.holds.push(hold(2, "thermal.relief", Priority::Safety, profile("balanced")));
        assert_eq!(
            knob.effective(),
            (Some(profile("balanced")), Source::Holds(Priority::Safety, vec!["thermal.relief".to_owned()]))
        );
    }

    #[test]
    fn holds_of_one_priority_merge() {
        let mut knob = Knob::new("power.profile", Merge::LowestPower);
        knob.holds.push(hold(1, "a", Priority::Automatic, profile("balanced")));
        knob.holds.push(hold(2, "b", Priority::Automatic, profile("power-saver")));
        assert_eq!(knob.effective().0, Some(profile("power-saver")));
    }
}
