//! Watches the system package databases: an install or a sync outside Settings (`pacman -U`, `pacman -Syu`,
//! `apt upgrade`) makes the last check's list stale, so a quiet period after such a change starts a new check.

use std::ffi::OsStr;
use std::io;
use std::mem::MaybeUninit;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::time::{Duration, Instant};

use rustix::fs::inotify;
use tokio::io::unix::AsyncFd;
use zbus::Connection;

use crate::service::{self, Update};

/// A package transaction writes the database many times; the check waits until it has been quiet this long.
const SETTLE: Duration = Duration::from_secs(5);

/// One watched directory: `names` limits the events to those file names (empty: every name).
pub struct Watch {
    pub dir: &'static str,
    pub names: &'static [&'static str],
    pub suffix: &'static str,
}

impl Watch {
    fn matches(&self, name: &OsStr) -> bool {
        let bytes = name.as_bytes();
        (self.names.is_empty() || self.names.iter().any(|n| n.as_bytes() == bytes))
            && bytes.ends_with(self.suffix.as_bytes())
    }
}

/// Runs for the life of the service. A directory that does not exist is skipped.
pub async fn run(conn: Connection, watches: &'static [Watch]) -> io::Result<()> {
    let fd = inotify::init(inotify::CreateFlags::CLOEXEC | inotify::CreateFlags::NONBLOCK)?;
    let flags = inotify::WatchFlags::CREATE
        | inotify::WatchFlags::DELETE
        | inotify::WatchFlags::MOVED_TO
        | inotify::WatchFlags::MOVED_FROM
        | inotify::WatchFlags::CLOSE_WRITE;
    let mut wds = Vec::new();
    for watch in watches {
        if Path::new(watch.dir).is_dir() {
            wds.push((inotify::add_watch(&fd, watch.dir, flags)?, watch));
        }
    }
    if wds.is_empty() {
        return Ok(());
    }
    // An OwnedFd keeps one open fd for its whole life, which is the condition of the unsafe AsyncFd::register.
    #[allow(deprecated)]
    let fd = AsyncFd::new(fd)?;
    let mut buf = [MaybeUninit::<u8>::uninit(); 4096];
    let mut changed: Option<Instant> = None;
    loop {
        let wait = changed.map(|at| (at + SETTLE).saturating_duration_since(Instant::now()));
        let ready = match wait {
            Some(left) => tokio::time::timeout(left, fd.readable()).await.ok(),
            None => Some(fd.readable().await),
        };
        let Some(ready) = ready else {
            let at = changed.take().unwrap_or_else(Instant::now);
            if let Ok(iface) = conn.object_server().interface::<_, Update>(service::PATH).await
                && !iface.get().await.check_after_change(&conn, at).await
            {
                // A check that began before the change still runs: try again when it has ended.
                changed = Some(at);
                tokio::time::sleep(SETTLE).await;
            }
            continue;
        };
        let mut ready = ready?;
        let mut events = inotify::Reader::new(ready.get_inner(), &mut buf);
        loop {
            match events.next() {
                Ok(event) => {
                    let overflow = event.events().contains(inotify::ReadFlags::QUEUE_OVERFLOW);
                    let relevant = event.file_name().is_some_and(|name| {
                        let name = OsStr::from_bytes(name.to_bytes());
                        wds.iter().any(|(wd, w)| *wd == event.wd() && w.matches(name))
                    });
                    if overflow || relevant {
                        changed = Some(Instant::now());
                    }
                }
                Err(rustix::io::Errno::AGAIN) => break,
                Err(e) => return Err(e.into()),
            }
        }
        ready.clear_ready();
    }
}
