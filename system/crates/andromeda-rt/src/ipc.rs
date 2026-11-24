//! Local Unix sockets: the Rust side of `protocol/ipc_channel.h`. A peer must run as this user and give a pidfd.

use std::io;
use std::os::fd::OwnedFd;
use std::sync::Arc;

use nix::sys::socket::{getsockopt, sockopt};
use nix::unistd::geteuid;
use tokio::io::{AsyncBufRead, AsyncBufReadExt};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

/// A connection from a process of this user. `pidfd` pins that process: `pid` alone can name a new process after the
/// peer exits.
pub struct Peer {
    pub stream: UnixStream,
    pub pid: i32,
    pub pidfd: OwnedFd,
}

/// The other end of a connected stream, when it runs as this user and gives a pidfd (`SO_PEERPIDFD`, Linux >= 6.5).
pub fn check_peer(stream: &UnixStream) -> io::Result<(i32, OwnedFd)> {
    let cred = getsockopt(stream, sockopt::PeerCredentials)?;
    if cred.uid() != geteuid().as_raw() {
        return Err(io::Error::new(io::ErrorKind::PermissionDenied, format!("peer runs as uid {}", cred.uid())));
    }
    Ok((cred.pid(), getsockopt(stream, sockopt::PeerPidfd)?))
}

/// The next connection from this user; a connection that fails `check_peer` is closed and logged.
pub async fn accept_peer(listener: &UnixListener) -> io::Result<Peer> {
    loop {
        let (stream, _) = listener.accept().await?;
        match check_peer(&stream) {
            Ok((pid, pidfd)) => return Ok(Peer { stream, pid, pidfd }),
            Err(e) => log::warn!("refused a connection: {e}"),
        }
    }
}

/// Bounds the open connections of one server: a connection holds its permit until it ends.
#[derive(Clone)]
pub struct ClientSlots(Arc<Semaphore>);

impl ClientSlots {
    pub fn new(max: usize) -> Self {
        Self(Arc::new(Semaphore::new(max)))
    }

    pub fn try_acquire(&self) -> Option<OwnedSemaphorePermit> {
        Arc::clone(&self.0).try_acquire_owned().ok()
    }
}

pub enum Line {
    Complete,
    TooLong,
    Eof,
}

/// Reads one line into `buf` without the newline, at most `cap` bytes. A longer line is skipped up to its newline,
/// so memory stays bounded and the next line still reads.
pub async fn read_line<R: AsyncBufRead + Unpin>(reader: &mut R, buf: &mut Vec<u8>, cap: usize) -> io::Result<Line> {
    buf.clear();
    let mut too_long = false;
    loop {
        let chunk = reader.fill_buf().await?;
        if chunk.is_empty() {
            return Ok(Line::Eof);
        }
        let (take, done) = chunk.iter().position(|&b| b == b'\n').map_or((chunk.len(), false), |i| (i, true));
        if !too_long {
            if buf.len() + take > cap {
                too_long = true;
                buf.clear();
            } else {
                buf.extend_from_slice(&chunk[..take]);
            }
        }
        reader.consume(take + usize::from(done));
        if done {
            return Ok(if too_long { Line::TooLong } else { Line::Complete });
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn accepts_this_process_with_a_pidfd() {
        use std::os::linux::net::SocketAddrExt;
        let name = format!("andromeda-rt-ipc-{}", std::process::id());
        let addr = std::os::unix::net::SocketAddr::from_abstract_name(name.as_bytes()).unwrap();
        let std_listener = std::os::unix::net::UnixListener::bind_addr(&addr).unwrap();
        std_listener.set_nonblocking(true).unwrap();
        let listener = UnixListener::from_std(std_listener).unwrap();
        let client = std::os::unix::net::UnixStream::connect_addr(&addr).unwrap();
        client.set_nonblocking(true).unwrap();
        let peer = accept_peer(&listener).await.unwrap();
        assert_eq!(peer.pid, i32::try_from(std::process::id()).unwrap());
        let (pid, _) = check_peer(&UnixStream::from_std(client).unwrap()).unwrap();
        assert_eq!(pid, peer.pid);
    }

    #[test]
    fn slots_bound_open_connections() {
        let slots = ClientSlots::new(2);
        let a = slots.try_acquire();
        let b = slots.try_acquire();
        assert!(a.is_some() && b.is_some() && slots.try_acquire().is_none());
        drop(a);
        assert!(slots.try_acquire().is_some());
    }
}
