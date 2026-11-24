//! andromeda-update-helper: root actions for Software Update (docs/system.md §1, U2). System bus,
//! `org.andromeda.UpdateHelper1`, D-Bus activated through systemd; exits after 5 idle minutes.

mod guard;
mod helper;
mod migrations;
mod snapshot;
#[cfg(feature = "apt")]
#[path = "transaction_apt.rs"]
mod transaction;
#[cfg(feature = "arch")]
mod transaction;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use anyhow::Result;

const NAME: &str = "org.andromeda.UpdateHelper1";
const IDLE_EXIT: Duration = Duration::from_secs(300);

fn main() -> Result<()> {
    andromeda_rt::init_log();
    // systemd sets STATE_DIRECTORY from StateDirectory=.
    let state_dir: std::path::PathBuf =
        std::env::var_os("STATE_DIRECTORY").map_or_else(|| "/var/lib/andromeda-update".into(), Into::into);
    // The package's install script and pacman hook.
    let migrations_dir = std::path::Path::new(migrations::DIR);
    match std::env::args().nth(1).as_deref() {
        Some("--seed-migrations") => migrations::seed(migrations_dir, &state_dir, true),
        Some("--run-migrations") => migrations::run(migrations_dir, &state_dir, true).map(|_| ()),
        // andromeda-update-recover.service at boot. SIGTERM only cancels the token, which recover ignores: a stop
        // waits.
        Some("--recover") => {
            andromeda_rt::run(
                1,
                |_| async move { tokio::task::spawn_blocking(move || guard::recover(&state_dir)).await? },
            )
        }
        _ => andromeda_rt::run(andromeda_rt::BLOCKING_THREADS, |shutdown| serve(shutdown, state_dir)),
    }
}

async fn serve(shutdown: andromeda_rt::Shutdown, state_dir: std::path::PathBuf) -> Result<()> {
    let running = Arc::new(AtomicBool::new(false));
    let last_call = Arc::new(AtomicU64::new(0));
    let started = Instant::now();
    let helper = helper::Helper { state_dir, running: running.clone(), last_call: last_call.clone(), started };
    let _conn = zbus::connection::Builder::system().serve_at(helper::PATH, helper).name(NAME).build().await?;
    log::info!("serving {NAME}");
    // A stop (shutdown, package upgrade) waits for a running commit: KillMode=mixed leaves its children alone and
    // TimeoutStopSec covers the longest commit.
    let mut stopping = false;
    let mut tick = tokio::time::interval(Duration::from_secs(30));
    loop {
        tokio::select! {
            _ = tick.tick() => {}
            () = shutdown.cancelled(), if !stopping => {
                log::info!("stop requested: exits once no update runs");
                stopping = true;
            }
        }
        let since_call = started.elapsed().saturating_sub(Duration::from_secs(last_call.load(Ordering::Relaxed)));
        if !running.load(Ordering::Relaxed) && (stopping || since_call > IDLE_EXIT) {
            return Ok(());
        }
    }
}
