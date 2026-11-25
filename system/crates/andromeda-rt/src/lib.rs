//! The runtime every andromeda daemon runs on (AR3): one current-thread tokio runtime whose blocking pool has a cap,
//! and one shutdown token that async code awaits and blocking closures poll.

pub mod backoff;
#[cfg(feature = "bus")]
pub mod bus;
pub mod ipc;
pub mod logged;
#[cfg(feature = "notify")]
pub mod notify;

use std::future::Future;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use tokio::signal::unix::{SignalKind, signal};
use tokio::sync::watch;

/// Blocking threads a daemon gets unless it states its own cap.
pub const BLOCKING_THREADS: usize = 8;
/// How long `run` waits for blocking work after the main future returns; the rest is dropped with the process.
pub const DRAIN: Duration = Duration::from_secs(3);

/// A shutdown request, shared by clones. Async code awaits `cancelled()`; blocking closures poll `flag()`.
#[derive(Clone)]
pub struct Shutdown {
    flag: Arc<AtomicBool>,
    tx: Arc<watch::Sender<bool>>,
}

impl Default for Shutdown {
    fn default() -> Self {
        Self::new()
    }
}

impl Shutdown {
    #[must_use]
    pub fn new() -> Self {
        Self { flag: Arc::new(AtomicBool::new(false)), tx: Arc::new(watch::channel(false).0) }
    }

    pub fn cancel(&self) {
        self.flag.store(true, Ordering::Relaxed);
        self.tx.send_replace(true);
    }

    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.flag.load(Ordering::Relaxed)
    }

    pub async fn cancelled(&self) {
        let mut rx = self.tx.subscribe();
        // The sender lives in self: changed() fails only after every Shutdown is gone.
        while !*rx.borrow_and_update() {
            if rx.changed().await.is_err() {
                return;
            }
        }
    }

    /// The flag a blocking closure checks between steps.
    #[must_use]
    pub fn flag(&self) -> Arc<AtomicBool> {
        self.flag.clone()
    }

    /// SIGTERM or SIGINT cancels. Call inside the runtime.
    pub fn cancel_on_signals(&self) -> std::io::Result<()> {
        let mut term = signal(SignalKind::terminate())?;
        let mut int = signal(SignalKind::interrupt())?;
        let this = self.clone();
        tokio::spawn(async move {
            tokio::select! {
                _ = term.recv() => {}
                _ = int.recv() => {}
            }
            this.cancel();
        });
        Ok(())
    }
}

/// A current-thread runtime with at most `blocking_threads` threads for `spawn_blocking` and `tokio::fs`.
pub fn runtime(blocking_threads: usize) -> std::io::Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_current_thread().enable_all().max_blocking_threads(blocking_threads).build()
}

/// `env_logger` at `info` unless `RUST_LOG` says otherwise.
pub fn init_log() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
}

/// Runs a daemon: `runtime(blocking_threads)`, a `Shutdown` that SIGTERM and SIGINT cancel (they no longer end the
/// process, so `main` must return on `cancelled()`), then `main(shutdown)`. Blocking work still running after `main`
/// returns gets `DRAIN`, then the runtime drops it.
pub fn run<F, Fut>(blocking_threads: usize, main: F) -> anyhow::Result<()>
where
    F: FnOnce(Shutdown) -> Fut,
    Fut: Future<Output = anyhow::Result<()>>,
{
    let rt = runtime(blocking_threads)?;
    let shutdown = Shutdown::new();
    let result = rt.block_on(async {
        shutdown.cancel_on_signals()?;
        main(shutdown.clone()).await
    });
    shutdown.cancel();
    rt.shutdown_timeout(DRAIN);
    result
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;
    use std::time::Instant;

    #[test]
    fn blocking_pool_never_exceeds_its_cap() {
        let rt = runtime(2).unwrap();
        let running = Arc::new(AtomicUsize::new(0));
        let peak = Arc::new(AtomicUsize::new(0));
        rt.block_on(async {
            let jobs: Vec<_> = (0..6)
                .map(|_| {
                    let (running, peak) = (running.clone(), peak.clone());
                    tokio::task::spawn_blocking(move || {
                        let now = running.fetch_add(1, Ordering::SeqCst) + 1;
                        peak.fetch_max(now, Ordering::SeqCst);
                        std::thread::sleep(Duration::from_millis(30));
                        running.fetch_sub(1, Ordering::SeqCst);
                    })
                })
                .collect();
            for job in jobs {
                job.await.unwrap();
            }
        });
        assert_eq!(peak.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn cancel_reaches_async_waiters_and_blocking_closures() {
        let rt = runtime(2).unwrap();
        let shutdown = Shutdown::new();
        rt.block_on(async {
            let flag = shutdown.flag();
            let blocking = tokio::task::spawn_blocking(move || {
                let start = Instant::now();
                while !flag.load(Ordering::Relaxed) && start.elapsed() < Duration::from_secs(5) {
                    std::thread::sleep(Duration::from_millis(1));
                }
                start.elapsed()
            });
            let waiter = tokio::spawn({
                let shutdown = shutdown.clone();
                async move { shutdown.cancelled().await }
            });
            tokio::time::sleep(Duration::from_millis(20)).await;
            assert!(!shutdown.is_cancelled());
            shutdown.cancel();
            tokio::time::timeout(Duration::from_secs(1), waiter).await.unwrap().unwrap();
            assert!(blocking.await.unwrap() < Duration::from_secs(1));
            // A waiter that starts after the cancel returns at once.
            tokio::time::timeout(Duration::from_millis(100), shutdown.cancelled()).await.unwrap();
        });
    }
}
