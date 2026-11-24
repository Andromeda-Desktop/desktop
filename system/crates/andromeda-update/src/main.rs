//! andromeda-update: software update checks for the Settings page (docs/system.md §1, U1). Session bus,
//! `org.andromeda.Update1`, D-Bus activated; no root. Installs are the root helper's job (U2).

mod flatpak;
mod fwupd;
#[cfg(feature = "apt")]
#[path = "apt.rs"]
mod packages;
#[cfg(feature = "arch")]
#[path = "pacman.rs"]
mod packages;
mod service;
mod snap;

use std::path::PathBuf;

use anyhow::{Context, Result};

const NAME: &str = "org.andromeda.Update1";

fn xdg(var: &str, fallback: &str) -> Result<PathBuf> {
    std::env::var_os(var)
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(fallback)))
        .context("no HOME")
}

fn main() -> Result<()> {
    andromeda_rt::init_log();
    andromeda_rt::run(andromeda_rt::BLOCKING_THREADS, serve)
}

async fn serve(shutdown: andromeda_rt::Shutdown) -> Result<()> {
    let cache = xdg("XDG_CACHE_HOME", ".cache")?.join("andromeda/update");
    let state = xdg("XDG_STATE_HOME", ".local/state")?.join("andromeda/update");
    let _conn = zbus::connection::Builder::session()
        .serve_at(service::PATH, service::Update::new(cache, state))
        .name(NAME)
        .build()
        .await?;
    log::info!("serving {NAME}");
    shutdown.cancelled().await;
    Ok(())
}
