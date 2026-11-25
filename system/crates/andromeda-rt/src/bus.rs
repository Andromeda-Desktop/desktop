//! D-Bus callers: the Rust side of `libdesk-system/src/dbus/caller.h`.

use std::os::fd::{AsFd, OwnedFd};

use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use zbus::Connection;
use zbus::fdo::{DBusProxy, Error, Result};
use zbus::message::Header;
use zbus::names::BusName;

/// The process behind a D-Bus call. `pidfd` pins it: `pid` alone can name a new process after the caller exits.
pub struct Caller {
    pub uid: u32,
    pub pid: u32,
    pub pidfd: OwnedFd,
}

impl Caller {
    /// True while the process runs. Check it after a lookup by `pid`, so the answer still names this caller.
    pub fn alive(&self) -> bool {
        // A pidfd becomes readable when its process exits.
        let mut fds = [PollFd::new(self.pidfd.as_fd(), PollFlags::POLLIN)];
        matches!(poll(&mut fds, PollTimeout::ZERO), Ok(0))
    }
}

/// The sender of the call with `header`, from one `GetConnectionCredentials` round trip. A bus that gives no pidfd
/// (dbus-daemon < 1.15.6) is refused.
pub async fn caller(conn: &Connection, header: &Header<'_>) -> Result<Caller> {
    let sender = header.sender().ok_or_else(|| Error::AccessDenied("call without sender".into()))?;
    let creds = DBusProxy::new(conn).await?.get_connection_credentials(BusName::Unique(sender.clone())).await?;
    let (Some(uid), Some(pid)) = (creds.unix_user_id(), creds.process_id()) else {
        return Err(Error::AccessDenied(format!("no credentials for {sender}")));
    };
    let pidfd = creds.into_process_fd().ok_or_else(|| Error::AccessDenied(format!("no pidfd for {sender}")))?;
    Ok(Caller { uid, pid, pidfd: pidfd.into() })
}

#[cfg(test)]
mod tests {
    use std::io::{BufRead, BufReader};
    use std::process::{Command, Stdio};

    use super::*;

    #[tokio::test]
    async fn a_caller_is_not_alive_after_its_process_exits() {
        let mut daemon = Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--print-address=1"])
            .stdout(Stdio::piped())
            .spawn()
            .expect("dbus-daemon");
        let mut address = String::new();
        BufReader::new(daemon.stdout.take().unwrap()).read_line(&mut address).unwrap();
        let conn = zbus::connection::Builder::address(address.trim()).build().await.unwrap();
        // The bus daemon is a peer whose process the test controls.
        let creds = DBusProxy::new(&conn)
            .await
            .unwrap()
            .get_connection_credentials(BusName::try_from("org.freedesktop.DBus").unwrap())
            .await
            .unwrap();
        let caller = Caller {
            uid: creds.unix_user_id().unwrap(),
            pid: creds.process_id().unwrap(),
            pidfd: creds.into_process_fd().unwrap().into(),
        };
        assert!(caller.alive());
        daemon.kill().unwrap();
        daemon.wait().unwrap();
        assert!(!caller.alive());
    }
}
