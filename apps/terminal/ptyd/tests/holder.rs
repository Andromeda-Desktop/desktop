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
        self.try_recv(TIMEOUT).expect("no message from the holder")
    }

    fn try_recv(&mut self, wait: Duration) -> Option<(HolderMsg, Option<OwnedFd>)> {
        let deadline = Instant::now() + wait;
        loop {
            if let Some((tag, payload, used)) = split_frame(&self.buf).unwrap() {
                let msg = HolderMsg::decode(tag, payload).unwrap();
                self.buf.drain(..used);
                let fd = matches!(msg, HolderMsg::Spawned { .. } | HolderMsg::Attached { .. })
                    .then(|| self.fds.pop_front())
                    .flatten();
                return Some((msg, fd));
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return None;
            }
            self.stream.set_read_timeout(Some(left)).unwrap();
            let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(1))];
            let mut control = RecvAncillaryBuffer::new(&mut space);
            let mut chunk = [0u8; 65536];
            let n = match recvmsg(
                &self.stream,
                &mut [std::io::IoSliceMut::new(&mut chunk)],
                &mut control,
                RecvFlags::CMSG_CLOEXEC,
            ) {
                Ok(r) => r.bytes,
                Err(rustix::io::Errno::AGAIN | rustix::io::Errno::INTR) => continue,
                Err(e) => panic!("recvmsg: {e}"),
            };
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
fn close_holds_when_the_client_hangs_up_with_unread_output() {
    let (_dir, path) = start(Config::default());
    let mut c = TestClient::connect(&path);
    let (id, _fd) = c.spawn(&["sh", "-c", "echo pid=$$; exec yes"]);
    let seen = c.read_until(id, 0, Vec::new(), |s| contains(s, "\n"));
    let text = String::from_utf8(seen).unwrap();
    let pid = text.lines().next().unwrap().strip_prefix("pid=").unwrap().to_string();
    let unread = || {
        let mut b = [MaybeUninit::uninit(); 1];
        rustix::net::recv(&c.stream, &mut b, RecvFlags::PEEK | RecvFlags::DONTWAIT).is_ok()
    };
    assert!(wait_for(unread, TIMEOUT));
    c.send(ClientMsg::Close { id });
    drop(c);
    assert!(
        wait_for(|| !Path::new(&format!("/proc/{pid}")).exists(), TIMEOUT),
        "child {pid} still exists"
    );
}

#[test]
fn close_kills_a_child_that_ignores_sighup() {
    let (_dir, path) = start(Config {
        close_grace: Duration::from_millis(100),
        ..Config::default()
    });
    let mut c = TestClient::connect(&path);
    let (id, _fd) = c.spawn(&["sh", "-c", "trap '' HUP; echo pid=$$; exec sleep 100"]);
    let seen = c.read_until(id, 0, Vec::new(), |s| contains(s, "\n"));
    let text = String::from_utf8(seen).unwrap();
    let pid = text.trim().strip_prefix("pid=").unwrap().to_string();
    c.send(ClientMsg::Close { id });
    assert!(
        wait_for(|| !Path::new(&format!("/proc/{pid}")).exists(), TIMEOUT),
        "child {pid} ignored SIGHUP and still exists"
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

#[derive(Clone, Copy, PartialEq, Debug)]
enum Owner {
    Client(usize),
    Orphaned,
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Life {
    Running,
    ExitSent,
    Exited,
}

struct ModelSession {
    owner: Owner,
    life: Life,
    pid: u32,
    /// Every byte of the stream from offset 0, as the clients received it.
    history: Vec<u8>,
    tokens: Vec<u32>,
}

/// Drives random event sequences against one holder and checks its invariants after each event.
struct Model {
    path: PathBuf,
    rng: u64,
    clients: Vec<Option<(TestClient, std::collections::HashMap<u64, OwnedFd>)>>,
    observer: TestClient,
    sessions: std::collections::BTreeMap<u64, ModelSession>,
    gone: std::collections::HashSet<u64>,
    next_token: u32,
    marker: PathBuf,
    relaunches: u64,
    /// Whether a running orphan existed in each of the last few steps; a relaunch runs asynchronously.
    orphan_window: VecDeque<bool>,
}

impl Model {
    fn pick(&mut self, n: usize) -> usize {
        self.rng ^= self.rng << 13;
        self.rng ^= self.rng >> 7;
        self.rng ^= self.rng << 17;
        (self.rng % n as u64) as usize
    }

    fn choose<T: Copy>(&mut self, items: &[T]) -> Option<T> {
        (!items.is_empty()).then(|| items[self.pick(items.len())])
    }

    fn live_clients(&self) -> Vec<usize> {
        (0..self.clients.len())
            .filter(|&i| self.clients[i].is_some())
            .collect()
    }

    fn sessions_where(&self, f: impl Fn(&ModelSession) -> bool) -> Vec<u64> {
        self.sessions
            .iter()
            .filter(|(_, s)| f(s))
            .map(|(&id, _)| id)
            .collect()
    }

    fn client(&mut self, slot: usize) -> &mut TestClient {
        &mut self.clients[slot].as_mut().unwrap().0
    }

    fn owned_fd(&self, id: u64) -> &OwnedFd {
        let Owner::Client(slot) = self.sessions[&id].owner else {
            panic!("session {id} has no client")
        };
        &self.clients[slot].as_ref().unwrap().1[&id]
    }

    /// Applies a stream message to the model; returns a reply message to the caller.
    fn handle(&mut self, slot: usize, msg: HolderMsg) -> Option<HolderMsg> {
        let id = match &msg {
            HolderMsg::Output { id, .. }
            | HolderMsg::WantSnapshot { id }
            | HolderMsg::Exited { id, .. } => *id,
            _ => return Some(msg),
        };
        if self.gone.contains(&id) {
            return None;
        }
        let s = self
            .sessions
            .get_mut(&id)
            .expect("message for an unknown session");
        assert_eq!(
            s.owner,
            Owner::Client(slot),
            "session {id} message went to client {slot}"
        );
        match msg {
            HolderMsg::Output { offset, bytes, .. } => {
                assert_eq!(
                    offset,
                    s.history.len() as u64,
                    "session {id}: output gap or duplicate"
                );
                s.history.extend_from_slice(&bytes);
            }
            HolderMsg::WantSnapshot { .. } => {
                let snap = ClientMsg::Snapshot {
                    id,
                    offset: s.history.len() as u64,
                    blob: s.history.clone(),
                };
                self.client(slot).send(snap);
            }
            HolderMsg::Exited { .. } => {
                assert_eq!(s.life, Life::ExitSent, "session {id} exited unasked");
                s.life = Life::Exited;
            }
            _ => unreachable!(),
        }
        None
    }

    fn pump(&mut self, slot: usize, wait: Duration) -> Option<(HolderMsg, Option<OwnedFd>)> {
        let (msg, fd) = self.client(slot).try_recv(wait)?;
        self.handle(slot, msg).map(|m| (m, fd))
    }

    /// Handles stream messages until none arrives within `wait`.
    fn drain(&mut self, slot: usize, wait: Duration) {
        while let Some((msg, _)) = self.client(slot).try_recv(wait) {
            if let Some(reply) = self.handle(slot, msg) {
                panic!("unexpected reply {reply:?}");
            }
        }
    }

    fn reply(&mut self, slot: usize) -> (HolderMsg, Option<OwnedFd>) {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            assert!(!left.is_zero(), "no reply for client {slot}");
            if let Some(r) = self.pump(slot, left) {
                return r;
            }
        }
    }

    fn pump_until(&mut self, slot: usize, what: &str, done: impl Fn(&Self) -> bool) {
        let deadline = Instant::now() + TIMEOUT;
        while !done(self) {
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            if let Some((msg, _)) = self.pump(slot, Duration::from_millis(50)) {
                panic!("unexpected reply {msg:?}");
            }
        }
    }

    fn token_counts_ok(s: &ModelSession, exact: bool) -> bool {
        s.tokens.iter().all(|t| {
            let n = s
                .history
                .windows(token(*t).len())
                .filter(|w| *w == token(*t).as_bytes())
                .count();
            if exact { n == 2 } else { n >= 2 }
        })
    }

    fn step(&mut self) {
        let clients = self.live_clients();
        let attached = self.sessions_where(|s| s.owner != Owner::Orphaned);
        let running =
            self.sessions_where(|s| s.owner != Owner::Orphaned && s.life == Life::Running);
        let orphans = self.sessions_where(|s| s.owner == Owner::Orphaned);
        match self.pick(8) {
            0 if clients.len() < 3 => {
                let c = TestClient::connect(&self.path);
                self.clients.push(Some((c, Default::default())));
            }
            1 => {
                let Some(slot) = self.choose(&clients) else {
                    return;
                };
                self.clients[slot] = None;
                for (&id, s) in self.sessions.iter_mut() {
                    if s.owner == Owner::Client(slot) {
                        s.owner = Owner::Orphaned;
                        if s.life == Life::Exited {
                            self.gone.insert(id);
                        }
                    }
                }
                self.sessions.retain(|id, _| !self.gone.contains(id));
            }
            2 => {
                let Some(slot) = self.choose(&clients) else {
                    return;
                };
                self.client(slot).send(ClientMsg::Spawn {
                    req: 1,
                    size: WinSize {
                        cols: 80,
                        rows: 24,
                        xpixel: 0,
                        ypixel: 0,
                    },
                    cwd: String::new(),
                    argv: ["sh", "-c", "echo P$$P; exec cat"]
                        .map(String::from)
                        .to_vec(),
                    env: vec!["PATH=/usr/bin:/bin".into()],
                });
                let (HolderMsg::Spawned { req: 1, id }, Some(fd)) = self.reply(slot) else {
                    panic!("spawn failed")
                };
                self.clients[slot].as_mut().unwrap().1.insert(id, fd);
                self.sessions.insert(
                    id,
                    ModelSession {
                        owner: Owner::Client(slot),
                        life: Life::Running,
                        pid: 0,
                        history: Vec::new(),
                        tokens: Vec::new(),
                    },
                );
                self.pump_until(slot, "pid line", |m| {
                    contains(&m.sessions[&id].history, "P\r\n")
                });
                let text = String::from_utf8_lossy(&self.sessions[&id].history).into_owned();
                let pid = text.split('P').nth(1).unwrap().parse().unwrap();
                self.sessions.get_mut(&id).unwrap().pid = pid;
            }
            3 | 4 => {
                let Some(id) = self.choose(&running) else {
                    return;
                };
                let t = self.next_token;
                self.next_token += 1;
                rustix::io::write(self.owned_fd(id), format!("{}\n", token(t)).as_bytes()).unwrap();
                self.sessions.get_mut(&id).unwrap().tokens.push(t);
            }
            5 => {
                let Some(id) = self.choose(&running) else {
                    return;
                };
                rustix::io::write(self.owned_fd(id), b"\x04").unwrap();
                self.sessions.get_mut(&id).unwrap().life = Life::ExitSent;
                let Owner::Client(slot) = self.sessions[&id].owner else {
                    unreachable!()
                };
                self.pump_until(slot, "exit", |m| m.sessions[&id].life == Life::Exited);
            }
            6 => {
                let (Some(id), Some(slot)) = (self.choose(&orphans), self.choose(&clients)) else {
                    return;
                };
                self.client(slot).send(ClientMsg::Attach { id });
                let (
                    HolderMsg::Attached {
                        id: i,
                        offset,
                        snapshot,
                        tail,
                    },
                    Some(fd),
                ) = self.reply(slot)
                else {
                    panic!("attach {id} failed")
                };
                assert_eq!((i, offset), (id, snapshot.len() as u64));
                let s = self.sessions.get_mut(&id).unwrap();
                let replay = [snapshot, tail].concat();
                assert!(
                    replay.starts_with(&s.history),
                    "session {id}: replay lost or changed bytes"
                );
                s.history = replay;
                s.owner = Owner::Client(slot);
                self.clients[slot].as_mut().unwrap().1.insert(id, fd);
            }
            7 => {
                let Some(id) = self.choose(&attached) else {
                    return;
                };
                let Owner::Client(slot) = self.sessions[&id].owner else {
                    unreachable!()
                };
                if self.pick(2) == 0 {
                    let blob = self.sessions[&id].history.clone();
                    self.client(slot).send(ClientMsg::Snapshot {
                        id,
                        offset: blob.len() as u64,
                        blob,
                    });
                    return;
                }
                self.client(slot).send(ClientMsg::Close { id });
                self.clients[slot].as_mut().unwrap().1.remove(&id);
                let s = self.sessions.remove(&id).unwrap();
                self.gone.insert(id);
                let proc = format!("/proc/{}", s.pid);
                assert!(
                    wait_for(|| !Path::new(&proc).exists(), TIMEOUT),
                    "Close left child {} running",
                    s.pid
                );
            }
            _ => {}
        }
    }

    /// Waits until the holder's orphan list equals the model's, and checks relaunches.
    fn check(&mut self) {
        for slot in self.live_clients() {
            self.drain(slot, Duration::from_millis(1));
        }
        let want = self.sessions_where(|s| s.owner == Owner::Orphaned);
        let deadline = Instant::now() + TIMEOUT;
        loop {
            self.observer.send(ClientMsg::List);
            let HolderMsg::Orphans { mut ids } = self.observer.recv().0 else {
                panic!("no orphan list")
            };
            ids.sort();
            if ids == want {
                break;
            }
            assert!(Instant::now() < deadline, "orphans {ids:?}, model {want:?}");
        }
        self.orphan_window.push_back(!want.is_empty());
        if self.orphan_window.len() > 5 {
            self.orphan_window.pop_front();
        }
        let n = std::fs::metadata(&self.marker).map_or(0, |m| m.len());
        if n > self.relaunches {
            assert!(
                self.orphan_window.iter().any(|&o| o),
                "relaunch without an orphan"
            );
            self.relaunches = n;
        }
    }

    /// Attaches every orphan and checks that each written line came back exactly once from the echo and once from cat.
    fn finish(&mut self) {
        if self.live_clients().is_empty() {
            self.clients
                .push(Some((TestClient::connect(&self.path), Default::default())));
        }
        let slot = self.live_clients()[0];
        for id in self.sessions_where(|s| s.owner == Owner::Orphaned) {
            self.client(slot).send(ClientMsg::Attach { id });
            let (HolderMsg::Attached { snapshot, tail, .. }, Some(fd)) = self.reply(slot) else {
                panic!("attach {id}")
            };
            let s = self.sessions.get_mut(&id).unwrap();
            let replay = [snapshot, tail].concat();
            assert!(
                replay.starts_with(&s.history),
                "session {id}: replay lost or changed bytes"
            );
            s.history = replay;
            s.owner = Owner::Client(slot);
            self.clients[slot].as_mut().unwrap().1.insert(id, fd);
        }
        for id in self.sessions_where(|_| true) {
            let Owner::Client(owner) = self.sessions[&id].owner else {
                unreachable!()
            };
            self.pump_until(owner, "every token", |m| {
                Self::token_counts_ok(&m.sessions[&id], false)
            });
        }
        for slot in self.live_clients() {
            self.drain(slot, Duration::from_millis(50));
        }
        for (id, s) in &self.sessions {
            assert!(
                Self::token_counts_ok(s, true),
                "session {id}: a line came back more than once"
            );
        }
    }
}

fn token(t: u32) -> String {
    format!("<{t}>")
}

#[test]
fn random_event_sequences_keep_the_holder_invariants() {
    let seeds: Vec<u64> = match std::env::var("PTYD_MODEL_SEED") {
        Ok(s) => vec![s.parse().unwrap()],
        Err(_) => (1..=6).collect(),
    };
    // A holder outlives its seed and may still relaunch into its directory.
    let mut dirs = Vec::new();
    for seed in seeds {
        eprintln!("PTYD_MODEL_SEED={seed}");
        let dir = tempfile::tempdir().unwrap();
        let marker = dir.path().join("relaunched");
        let (_d, path) = start(Config {
            relaunch: Some(vec![
                "sh".into(),
                "-c".into(),
                format!("echo >> {}", marker.display()),
            ]),
            relaunch_delay: Duration::from_millis(20),
            snapshot_at: 64,
            ..Config::default()
        });
        let mut m = Model {
            observer: TestClient::connect(&path),
            path,
            rng: seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1,
            clients: Vec::new(),
            sessions: Default::default(),
            gone: Default::default(),
            next_token: 0,
            marker,
            relaunches: 0,
            orphan_window: VecDeque::new(),
        };
        for _ in 0..120 {
            m.step();
            m.check();
        }
        m.finish();
        dirs.push((dir, _d));
    }
}
