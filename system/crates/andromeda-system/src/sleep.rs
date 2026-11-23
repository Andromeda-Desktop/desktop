//! System1 holds the only logind sleep delay. Session compositors register with `Login.HoldSleep`; after logind's
//! `PrepareForSleep(true)` the delay ends when every holder called `Login.SleepReady`, or when the budget ends.

use std::collections::BTreeSet;
use std::time::Duration;

use tokio::time::Instant;
use zbus::{Connection, OwnedFd};

/// logind's default `InhibitDelayMaxSec`, for when the property cannot be read.
const DEFAULT_DELAY_MAX: Duration = Duration::from_secs(5);
/// The delay ends this long before logind's own limit, so the release and its log line come from System1.
const MARGIN: Duration = Duration::from_millis(500);

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum Phase {
    #[default]
    Awake,
    Preparing,
    Released,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Hold(String),
    Gone(String),
    Ready(String),
    Prepare,
    Budget,
    Resume,
}

/// Holders by unique bus name. Invariant: `ready` is a subset of `holders`.
#[derive(Debug, Default)]
pub struct Relay {
    holders: BTreeSet<String>,
    ready: BTreeSet<String>,
    phase: Phase,
}

impl Relay {
    /// Applies `event`. `Some(missed)`: release the delay now; `missed` are the holders that did not call `SleepReady`.
    pub fn step(&mut self, event: Event) -> Option<Vec<String>> {
        match event {
            Event::Hold(name) => {
                self.holders.insert(name);
                None
            }
            Event::Gone(name) => {
                self.holders.remove(&name);
                self.ready.remove(&name);
                self.release_if_ready()
            }
            Event::Ready(name) => {
                if self.phase == Phase::Preparing && self.holders.contains(&name) {
                    self.ready.insert(name);
                }
                self.release_if_ready()
            }
            Event::Prepare if self.phase == Phase::Awake => {
                self.phase = Phase::Preparing;
                self.release_if_ready()
            }
            Event::Budget if self.phase == Phase::Preparing => {
                self.phase = Phase::Released;
                Some(self.holders.difference(&self.ready).cloned().collect())
            }
            Event::Prepare | Event::Budget => None,
            Event::Resume => {
                self.phase = Phase::Awake;
                self.ready.clear();
                None
            }
        }
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }

    fn release_if_ready(&mut self) -> Option<Vec<String>> {
        (self.phase == Phase::Preparing && self.ready.len() == self.holders.len()).then(|| {
            self.phase = Phase::Released;
            Vec::new()
        })
    }
}

/// The logind delay fd and the relay that decides when to close it.
pub struct Delay {
    relay: Relay,
    fd: Option<OwnedFd>,
    deadline: Option<Instant>,
}

impl Delay {
    pub async fn new(conn: &Connection) -> Self {
        let mut delay = Self { relay: Relay::default(), fd: None, deadline: None };
        delay.take(conn).await;
        delay
    }

    /// The budget deadline while a sleep waits for holders.
    pub fn deadline(&self) -> Option<Instant> {
        self.deadline
    }

    pub async fn apply(&mut self, conn: &Connection, event: Event) {
        let resume = event == Event::Resume;
        let prepare = event == Event::Prepare;
        if let Some(missed) = self.relay.step(event) {
            if !missed.is_empty() {
                log::warn!("login: sleep budget ended before SleepReady from {}", missed.join(", "));
            }
            self.fd = None;
            self.deadline = None;
        } else if prepare && self.relay.phase() == Phase::Preparing {
            self.deadline = Some(Instant::now() + budget(conn).await);
        }
        if resume {
            self.deadline = None;
            self.take(conn).await;
        }
    }

    async fn take(&mut self, conn: &Connection) {
        let args = ("sleep", "andromeda-system", "Session compositors lock the screen before sleep", "delay");
        self.fd = match crate::login::manager::<_, OwnedFd>(conn, "Inhibit", &args).await {
            Ok(fd) => Some(fd),
            Err(e) => {
                log::warn!("login: no logind sleep delay: {e}");
                None
            }
        };
    }
}

/// How long holders get after `PrepareForSleep(true)`: logind's `InhibitDelayMaxUSec` less a margin.
pub async fn budget(conn: &Connection) -> Duration {
    let max = match crate::login::manager_property::<u64>(conn, "InhibitDelayMaxUSec").await {
        Ok(usec) => Duration::from_micros(usec),
        Err(e) => {
            log::warn!("login: {e}; sleep budget uses logind's default");
            DEFAULT_DELAY_MAX
        }
    };
    max.saturating_sub(MARGIN)
}

#[cfg(test)]
mod tests {
    use super::*;

    const NAMES: [&str; 3] = [":1.1", ":1.2", ":1.3"];

    struct Rng(u64);

    impl Rng {
        fn below(&mut self, n: u8) -> u8 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0.to_le_bytes()[0] % n
        }
    }

    fn random_event(rng: &mut Rng) -> Event {
        let name = NAMES[usize::from(rng.below(3))].to_owned();
        match rng.below(8) {
            0 | 1 => Event::Hold(name),
            2 => Event::Gone(name),
            3 | 4 => Event::Ready(name),
            5 => Event::Prepare,
            6 => Event::Budget,
            _ => Event::Resume,
        }
    }

    /// Oracle from the event log alone: holders are names whose last Hold/Gone was a Hold; a holder is ready when
    /// it called Ready after the window's Prepare and after the Hold that made it a holder.
    #[test]
    fn random_sequences_release_only_when_every_holder_is_ready_or_the_budget_ends() {
        for seed in 1..=2000u64 {
            let mut rng = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
            let mut relay = Relay::default();
            let mut log: Vec<Event> = Vec::new();
            let mut window: Option<usize> = None;
            let mut released = false;
            for _ in 0..60 {
                let event = random_event(&mut rng);
                log.push(event.clone());
                let was_awake = window.is_none();
                match &event {
                    Event::Prepare if was_awake => {
                        window = Some(log.len() - 1);
                        released = false;
                    }
                    Event::Resume => window = None,
                    _ => {}
                }
                let out = relay.step(event.clone());
                let holders: BTreeSet<&str> = NAMES
                    .into_iter()
                    .filter(|n| {
                        log.iter().rev().find_map(|e| match e {
                            Event::Hold(h) if h == n => Some(true),
                            Event::Gone(h) if h == n => Some(false),
                            _ => None,
                        }) == Some(true)
                    })
                    .collect();
                let ready = |n: &str, start: usize| {
                    let gone = log.iter().rposition(|e| *e == Event::Gone(n.to_owned())).map_or(0, |i| i + 1);
                    let held =
                        gone + log[gone..].iter().position(|e| *e == Event::Hold(n.to_owned())).unwrap_or_default();
                    log[start.max(held)..].iter().any(|e| *e == Event::Ready(n.to_owned()))
                };
                let all_ready = |start: usize| holders.iter().all(|n| ready(n, start));
                match (window, out) {
                    (None, Some(_)) => panic!("seed {seed}: release while awake after {event:?}"),
                    (Some(_), Some(_)) if released => panic!("seed {seed}: second release in one sleep"),
                    (Some(start), Some(missed)) => {
                        let expect: Vec<String> =
                            holders.iter().filter(|n| !ready(n, start)).map(|n| (*n).to_owned()).collect();
                        assert_eq!(missed, expect, "seed {seed}: missed holders after {event:?}");
                        assert!(event == Event::Budget || all_ready(start), "seed {seed}: early release");
                        released = true;
                    }
                    (Some(start), None) => {
                        assert!(released || (event != Event::Budget && !all_ready(start)), "seed {seed}: no release");
                    }
                    (None, None) => {}
                }
            }
        }
    }
}
