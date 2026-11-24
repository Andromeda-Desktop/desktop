use std::os::fd::AsFd;
use std::os::unix::net::UnixListener;
use std::path::{Path, PathBuf};

use andromeda_agentd::Broker;
use anyhow::{Context, bail};

fn main() -> anyhow::Result<()> {
    andromeda_rt::init_log();
    let dir = PathBuf::from(std::env::var_os("XDG_RUNTIME_DIR").context("XDG_RUNTIME_DIR is not set")?);
    let listener = listener(&dir)?;
    listener.set_nonblocking(true)?;
    andromeda_rt::run(andromeda_rt::BLOCKING_THREADS, |shutdown| async move {
        let listener = tokio::net::UnixListener::from_std(listener)?;
        tokio::select! {
            result = Broker::default().run(listener, dir) => Ok(result?),
            () = shutdown.cancelled() => Ok(()),
        }
    })
}

fn listener(dir: &Path) -> anyhow::Result<UnixListener> {
    // Socket activation passes the socket as stdin (StandardInput=socket): taking LISTEN_FDS fd 3 needs unsafe code.
    let stdin = std::io::stdin();
    if rustix::net::sockopt::socket_acceptconn(&stdin).unwrap_or(false) {
        return Ok(UnixListener::from(stdin.as_fd().try_clone_to_owned()?));
    }
    let path = dir.join("andromeda-agentd.sock");
    if std::os::unix::net::UnixStream::connect(&path).is_ok() {
        bail!("another broker listens on {}", path.display());
    }
    let _ = std::fs::remove_file(&path);
    UnixListener::bind(&path).with_context(|| format!("bind {}", path.display()))
}
