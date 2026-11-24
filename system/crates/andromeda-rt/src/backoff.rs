//! Retry delays for reconnects and failed work (C12), the mirror of protocol/backoff.h: doubling from `first` to
//! `cap`, each spread by jitter so clients that failed together do not retry together.

use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct Backoff {
    first: Duration,
    cap: Duration,
    step: Duration,
}

impl Backoff {
    pub const fn new(first: Duration, cap: Duration) -> Self {
        Self { first, cap, step: first }
    }

    /// The wait before the next try: a random point in [step/2, step]. Each call doubles step up to `cap`.
    pub fn next_delay(&mut self) -> Duration {
        let step = self.step;
        self.step = (step * 2).min(self.cap);
        let half = u64::try_from((step / 2).as_nanos()).unwrap_or(u64::MAX);
        step.saturating_sub(Duration::from_nanos(random() % half.saturating_add(1)))
    }

    /// After a success: the next failure waits about `first` again.
    pub fn reset(&mut self) {
        self.step = self.first;
    }
}

/// std seeds every `RandomState` with new keys, so one hash of nothing is a fresh random number.
fn random() -> u64 {
    RandomState::new().build_hasher().finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn doubles_to_cap_within_jitter_and_resets() {
        let first = Duration::from_millis(100);
        let cap = Duration::from_millis(400);
        let mut backoff = Backoff::new(first, cap);
        for step in [100, 200, 400, 400, 400] {
            let step = Duration::from_millis(step);
            let delay = backoff.next_delay();
            assert!(delay >= step / 2 && delay <= step, "{delay:?} outside [{:?}, {step:?}]", step / 2);
        }
        backoff.reset();
        assert!(backoff.next_delay() <= first);
    }

    #[test]
    fn jitter_spreads_clients() {
        let delays: std::collections::HashSet<_> =
            (0..16).map(|_| Backoff::new(Duration::from_secs(1), Duration::from_secs(1)).next_delay()).collect();
        assert!(delays.len() > 1, "16 clients got the same delay");
    }
}
