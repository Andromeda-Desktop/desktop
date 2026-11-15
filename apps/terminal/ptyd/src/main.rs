//! andromeda-ptyd: holds terminal ptys so their programs outlive the terminal window process.
//!
//! Usage: andromeda-ptyd [--socket PATH] [--relaunch PROGRAM ARGS...]
//! Without --socket it takes the listening socket from systemd (LISTEN_FDS=1, fd 3).

use std::os::fd::{FromRawFd, OwnedFd};
use std::os::unix::net::UnixListener;

use andromeda_ptyd::holder::{Config, Holder};

fn systemd_listener() -> Option<UnixListener> {
    let ours = std::env::var("LISTEN_PID").ok()?.parse::<u32>().ok()? == std::process::id();
    if !ours || std::env::var("LISTEN_FDS").ok()? != "1" {
        return None;
    }
    // SAFETY: systemd passes exactly one listening socket as fd 3 (LISTEN_FDS=1, LISTEN_PID is us); nothing else owns it.
    #[allow(unsafe_code)]
    let fd = unsafe { OwnedFd::from_raw_fd(3) };
    Some(UnixListener::from(fd))
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mut socket = None;
    let mut config = Config::default();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--socket" => socket = args.next(),
            "--relaunch" => config.relaunch = Some(args.by_ref().collect()),
            other => {
                eprintln!("andromeda-ptyd: unknown argument {other}");
                std::process::exit(2);
            }
        }
    }
    let listener = match socket {
        Some(path) => {
            let _ = std::fs::remove_file(&path);
            UnixListener::bind(&path)
        }
        None => systemd_listener()
            .ok_or_else(|| std::io::Error::other("no --socket and no systemd socket")),
    };
    let result = listener
        .and_then(|l| Holder::new(l, config))
        .and_then(|mut h| h.run());
    if let Err(e) = result {
        eprintln!("andromeda-ptyd: {e}");
        std::process::exit(1);
    }
}
