//! andromeda-fileops: file operations for every app on the session bus (`org.andromeda.FileOps1`), so a copy
//! survives a closed window and all windows show the same jobs (docs/architecture-v2.md §12b). D-Bus activated;
//! exits after 10 min with no running job, and the undo history goes with it.

mod archive;
mod info;
mod jobs;
mod ops;
mod storage;
mod tags;
mod trash;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use andromeda_rt::Shutdown;
use anyhow::Result;

const NAME: &str = "org.andromeda.FileOps1";
const IDLE_EXIT: Duration = Duration::from_secs(600);
/// Copy jobs run on blocking threads: more jobs than this wait in line.
const BLOCKING_THREADS: usize = 16;

fn main() -> Result<()> {
    andromeda_rt::init_log();
    andromeda_rt::run(BLOCKING_THREADS, serve)
}

async fn serve(shutdown: Shutdown) -> Result<()> {
    let activity = Arc::new(Mutex::new((0usize, Some(Instant::now()))));
    let manager = jobs::Manager::new(activity.clone(), shutdown.flag());
    let _conn = zbus::connection::Builder::session().serve_at(jobs::PATH, manager).name(NAME).build().await?;
    log::info!("serving {NAME}");

    let mut tick = tokio::time::interval(Duration::from_secs(60));
    loop {
        tokio::select! {
            () = shutdown.cancelled() => {
                drain(&activity).await;
                return Ok(());
            }
            _ = tick.tick() => {
                let (running, last) = *activity.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                if running == 0 && last.is_some_and(|t| t.elapsed() >= IDLE_EXIT) {
                    log::info!("idle; exiting");
                    return Ok(());
                }
            }
        }
    }
}

/// Running jobs stop at their next check (the shutdown flag) and get 3 s; a copy stopped there leaves no partial file.
async fn drain(activity: &jobs::Activity) {
    for _ in 0..30 {
        if activity.lock().unwrap_or_else(std::sync::PoisonError::into_inner).0 == 0 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}
