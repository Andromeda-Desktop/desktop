use std::collections::VecDeque;
use std::io::Write;
use std::mem::MaybeUninit;
use std::os::fd::OwnedFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use andromeda_ptyd::holder::{Config, Holder};
use andromeda_ptyd::proto::{ClientMsg, HolderMsg, WinSize, split_frame};
use rustix::net::{RecvAncillaryBuffer, RecvAncillaryMessage, RecvFlags, recvmsg};

const TIMEOUT: Duration = Duration::from_secs(5);

fn start(config: Config) -> (tempfile::TempDir, PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("ptyd.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let config = Config {
        idle_exit: None,
        ..config
    };
    std::thread::spawn(move || Holder::new(listener, config).unwrap().run().unwrap());
    (dir, path)
}

struct TestClient {
    stream: UnixStream,
    buf: Vec<u8>,
    fds: VecDeque<OwnedFd>,
}

impl TestClient {
    fn connect(path: &Path) -> Self {
        TestClient {
            stream: UnixStream::connect(path).unwrap(),
            buf: Vec::new(),
            fds: VecDeque::new(),
        }
    }

    fn send(&mut self, msg: ClientMsg) {
        self.stream.write_all(&msg.encode()).unwrap();
    }

    /// Next message, with the pty fd it carries.
    fn recv(&mut self) -> (HolderMsg, Option<OwnedFd>) {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if let Some((tag, payload, used)) = split_frame(&self.buf).unwrap() {
                let msg = HolderMsg::decode(tag, payload).unwrap();
                self.buf.drain(..used);
                let fd = matches!(msg, HolderMsg::Spawned { .. } | HolderMsg::Attached { .. })
                    .then(|| self.fds.pop_front())
                    .flatten();
                return (msg, fd);
            }
            let left = deadline.saturating_duration_since(Instant::now());
            assert!(!left.is_zero(), "no message from the holder");
            self.stream.set_read_timeout(Some(left)).unwrap();
            let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(1))];
            let mut control = RecvAncillaryBuffer::new(&mut space);
            let mut chunk = [0u8; 65536];
            let n = recvmsg(
                &self.stream,
                &mut [std::io::IoSliceMut::new(&mut chunk)],
                &mut control,
                RecvFlags::CMSG_CLOEXEC,
            )
            .unwrap()
            .bytes;
            assert!(n > 0, "holder closed the connection");
            for msg in control.drain() {
                if let RecvAncillaryMessage::ScmRights(fds) = msg {
                    self.fds.extend(fds);
                }
            }
            self.buf.extend_from_slice(&chunk[..n]);
        }
    }

    fn spawn(&mut self, argv: &[&str]) -> (u64, OwnedFd) {
        self.send(ClientMsg::Spawn {
            req: 7,
            size: WinSize {
                cols: 80,
                rows: 24,
                xpixel: 0,
                ypixel: 0,
            },
            cwd: String::new(),
            argv: argv.iter().map(|s| s.to_string()).collect(),
            env: vec!["PATH=/usr/bin:/bin".into()],
        });
        match self.recv() {
            (HolderMsg::Spawned { req: 7, id }, Some(fd)) => (id, fd),
            other => panic!("expected Spawned with a fd, got {:?}", other.0),
        }
    }

    /// Reads output until `pred` holds for all bytes seen; answers WantSnapshot at the received offset.
    fn read_until(
        &mut self,
        id: u64,
        start: u64,
        mut seen: Vec<u8>,
        pred: impl Fn(&[u8]) -> bool,
    ) -> Vec<u8> {
        let mut end = start;
        while !pred(&seen) {
            match self.recv().0 {
                HolderMsg::Output {
                    id: i,
                    offset,
                    bytes,
                } if i == id => {
                    end = offset + bytes.len() as u64;
                    seen.extend_from_slice(&bytes);
                }
                HolderMsg::WantSnapshot { id: i } if i == id => self.send(ClientMsg::Snapshot {
                    id,
                    offset: end,
                    blob: b"snap".to_vec(),
                }),
                HolderMsg::Exited { .. } => panic!(
                    "exited before the expected output: {:?}",
                    String::from_utf8_lossy(&seen)
                ),
                _ => {}
            }
        }
        seen
    }
}

fn contains(hay: &[u8], needle: &str) -> bool {
    hay.windows(needle.len()).any(|w| w == needle.as_bytes())
}

fn wait_for(cond: impl Fn() -> bool, within: Duration) -> bool {
    let deadline = Instant::now() + within;
    while Instant::now() < deadline {
        if cond() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    cond()
}

/// The holder sees a client hang up asynchronously: ask until the orphans show.
fn wait_for_orphans(c: &mut TestClient) -> Vec<u64> {
    let deadline = Instant::now() + TIMEOUT;
    loop {
        c.send(ClientMsg::List);
        let HolderMsg::Orphans { ids } = c.recv().0 else {
            panic!("no orphan list")
        };
        if !ids.is_empty() || Instant::now() > deadline {
            return ids;
        }
    }
}

#[test]
fn spawn_streams_output_then_exit_status() {
    let (_dir, path) = start(Config::default());
    let mut c = TestClient::connect(&path);
    let (id, _fd) = c.spawn(&["sh", "-c", "printf hello; exit 3"]);
    let mut seen = Vec::new();
    let status = loop {
        match c.recv().0 {
            HolderMsg::Output { bytes, .. } => seen.extend_from_slice(&bytes),
            HolderMsg::Exited { id: i, status } if i == id => break status,
            _ => {}
        }
    };
    assert!(contains(&seen, "hello"));
    assert_eq!(status, 3 << 8);
}

#[test]
fn orphaned_session_replays_and_takes_input_after_reattach() {
    let (_dir, path) = start(Config::default());
    let mut first = TestClient::connect(&path);
    let (id, fd) = first.spawn(&["cat"]);
    rustix::io::write(&fd, b"one\n").unwrap();
    first.read_until(id, 0, Vec::new(), |s| contains(s, "one\r\none"));
    drop((first, fd));

    let mut second = TestClient::connect(&path);
    let listed = wait_for_orphans(&mut second);
    assert_eq!(listed, vec![id]);
    second.send(ClientMsg::Attach { id });
    let (
        HolderMsg::Attached {
            offset: 0,
            snapshot,
            tail,
            ..
        },
        Some(fd),
    ) = second.recv()
    else {
        panic!("not attached")
    };
    assert!(snapshot.is_empty());
    assert!(
        contains(&tail, "one\r\none"),
        "tail {:?}",
        String::from_utf8_lossy(&tail)
    );

    rustix::io::write(&fd, b"two\n").unwrap();
    let (HolderMsg::Output { offset, bytes, .. }, _) = second.recv() else {
        panic!("no output")
    };
    assert_eq!(offset, tail.len() as u64);
    let end = offset + bytes.len() as u64;
    second.read_until(id, end, [tail, bytes].concat(), |s| {
        contains(s, "two\r\ntwo")
    });
}

#[test]
fn snapshot_replaces_the_tail_it_covers() {
    let (_dir, path) = start(Config {
        snapshot_at: 8,
        ..Config::default()
    });
    let mut first = TestClient::connect(&path);
    let (id, _fd) = first.spawn(&["sh", "-c", "printf 0123456789; exec cat"]);
    first.read_until(id, 0, Vec::new(), |s| contains(s, "0123456789"));
    loop {
        if let HolderMsg::WantSnapshot { id: i } = first.recv().0 {
            first.send(ClientMsg::Snapshot {
                id: i,
                offset: 10,
                blob: b"snap".to_vec(),
            });
            break;
        }
    }
    drop(first);

    let mut second = TestClient::connect(&path);
    wait_for_orphans(&mut second);
    second.send(ClientMsg::Attach { id });
    let (
        HolderMsg::Attached {
            offset,
            snapshot,
            tail,
            ..
        },
        _,
    ) = second.recv()
    else {
        panic!("not attached")
    };
    assert_eq!(
        (offset, snapshot.as_slice(), tail.as_slice()),
        (10, b"snap".as_slice(), b"".as_slice())
    );
}

#[test]
fn orphan_blocks_the_child_at_the_tail_cap_and_loses_nothing() {
    let cap = 4096;
    let (dir, path) = start(Config {
        tail_cap: cap,
        snapshot_at: 1024,
        ..Config::default()
    });
    let done = dir.path().join("done");
    let script = format!(
        "head -c 100000 /dev/zero | tr '\\0' a; touch {}; exec cat",
        done.display()
    );
    let mut first = TestClient::connect(&path);
    let (id, _fd) = first.spawn(&["sh", "-c", &script]);
    drop((first, _fd));

    assert!(
        !wait_for(|| done.exists(), Duration::from_secs(1)),
        "child was not blocked"
    );
    let mut second = TestClient::connect(&path);
    wait_for_orphans(&mut second);
    second.send(ClientMsg::Attach { id });
    let (
        HolderMsg::Attached {
            offset: 0, tail, ..
        },
        _,
    ) = second.recv()
    else {
        panic!("not attached")
    };
    assert!(tail.len() <= cap, "tail {} over cap", tail.len());
    let end = tail.len() as u64;
    second.send(ClientMsg::Snapshot {
        id,
        offset: end,
        blob: Vec::new(),
    });
    let mut count = tail.len();
    let all = second.read_until(id, end, Vec::new(), |s| count + s.len() >= 100000);
    count += all.len();
    assert_eq!(count, 100000);
    assert!(wait_for(|| done.exists(), TIMEOUT));
}

#[test]
fn close_hangs_up_the_child() {
    let (_dir, path) = start(Config::default());
    let mut c = TestClient::connect(&path);
    let (id, _fd) = c.spawn(&["sh", "-c", "echo pid=$$; exec sleep 100"]);
    let seen = c.read_until(id, 0, Vec::new(), |s| contains(s, "\n"));
    let text = String::from_utf8(seen).unwrap();
    let pid = text.trim().strip_prefix("pid=").unwrap().to_string();
    c.send(ClientMsg::Close { id });
    drop(_fd);
    assert!(
        wait_for(|| !Path::new(&format!("/proc/{pid}")).exists(), TIMEOUT),
        "child {pid} still exists"
    );
}

#[test]
fn relaunch_runs_only_for_orphaned_running_sessions() {
    let dir = tempfile::tempdir().unwrap();
    let marker = dir.path().join("relaunched");
    let relaunch = Some(vec!["touch".to_string(), marker.display().to_string()]);
    let config = || Config {
        relaunch: relaunch.clone(),
        relaunch_delay: Duration::from_millis(20),
        ..Config::default()
    };

    let (_d1, closed_path) = start(config());
    let mut c = TestClient::connect(&closed_path);
    let (id, _fd) = c.spawn(&["cat"]);
    c.send(ClientMsg::Close { id });
    c.send(ClientMsg::List);
    assert_eq!(c.recv().0, HolderMsg::Orphans { ids: vec![] });
    drop(c);
    assert!(
        !wait_for(|| marker.exists(), Duration::from_millis(300)),
        "relaunched with no orphans"
    );

    let (_d2, crash_path) = start(config());
    let mut c = TestClient::connect(&crash_path);
    c.spawn(&["cat"]);
    drop(c);
    assert!(
        wait_for(|| marker.exists(), TIMEOUT),
        "no relaunch after a client crash"
    );
}
