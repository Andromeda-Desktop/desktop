//! The holder loop: one thread, one poll over the listener, the clients, the pty masters and the child pidfds.

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::ffi::OsString;
use std::io::{IoSlice, Read};
use std::mem::MaybeUninit;
use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::ffi::OsStringExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::os::unix::process::CommandExt;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use rustix::event::{PollFd, PollFlags, Timespec, poll};
use rustix::fs::{Mode, OFlags};
use rustix::net::{SendAncillaryBuffer, SendAncillaryMessage, SendFlags, sendmsg};
use rustix::process::{Pid, PidfdFlags, Signal};

use crate::proto::{ClientMsg, HolderMsg, WinSize, split_frame};

pub struct Config {
    /// Run when a client disconnects and leaves running sessions with no owner.
    pub relaunch: Option<Vec<String>>,
    pub relaunch_delay: Duration,
    /// Ask the attached client for a snapshot when the unsnapshotted tail reaches this size.
    pub snapshot_at: usize,
    /// Stop reading a pty while its tail is this large: the child blocks, and no output is lost.
    pub tail_cap: usize,
    /// Stop reading a pty while its client has this much unsent output queued.
    pub client_queue_cap: usize,
    /// Return when no client and no session was present for this long.
    pub idle_exit: Option<Duration>,
    /// A closed session's child that still runs this long after SIGHUP gets SIGKILL.
    pub close_grace: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            relaunch: None,
            relaunch_delay: Duration::from_secs(1),
            snapshot_at: 4 << 20,
            tail_cap: 8 << 20,
            client_queue_cap: 1 << 20,
            idle_exit: Some(Duration::from_secs(30)),
            close_grace: Duration::from_secs(3),
        }
    }
}

const READ_CHUNK: usize = 64 << 10;
const RELAUNCH_LIMIT: usize = 3;
const RELAUNCH_WINDOW: Duration = Duration::from_secs(60);

type ClientId = u64;
type SessionId = u64;

enum Owner {
    Attached(ClientId),
    Orphaned,
    /// The user closed it: the child got SIGHUP and gets SIGKILL at `kill_at` if it ignores that.
    Closing {
        kill_at: Instant,
    },
    /// Closed and the child got SIGKILL, or had already exited; the session goes when the child is reaped.
    Closed,
}

enum Life {
    Running {
        master: OwnedFd,
    },
    /// Reaped; other processes may still hold the pty, so output can still arrive.
    ChildGone {
        master: OwnedFd,
        status: i32,
    },
    /// The pty hung up or was closed; the child is not reaped yet.
    PtyGone,
    Exited {
        status: i32,
    },
}

struct Session {
    child: Child,
    /// None once the child is reaped.
    pidfd: Option<OwnedFd>,
    life: Life,
    owner: Owner,
    snapshot: Vec<u8>,
    snapshot_offset: u64,
    /// Output from `snapshot_offset` on.
    tail: Vec<u8>,
    want_sent_at: u64,
}

impl Session {
    fn read_offset(&self) -> u64 {
        self.snapshot_offset + self.tail.len() as u64
    }

    fn master(&self) -> Option<&OwnedFd> {
        match &self.life {
            Life::Running { master } | Life::ChildGone { master, .. } => Some(master),
            Life::PtyGone | Life::Exited { .. } => None,
        }
    }

    fn on_reaped(&mut self, status: i32) {
        self.pidfd = None;
        self.life = match std::mem::replace(&mut self.life, Life::PtyGone) {
            Life::Running { master } => Life::ChildGone { master, status },
            Life::PtyGone => Life::Exited { status },
            other => other,
        };
    }

    fn on_hangup(&mut self) {
        self.life = match std::mem::replace(&mut self.life, Life::PtyGone) {
            Life::Running { .. } => Life::PtyGone,
            Life::ChildGone { status, .. } => Life::Exited { status },
            other => other,
        };
    }
}

struct Outgoing {
    bytes: Vec<u8>,
    sent: usize,
    fd: Option<OwnedFd>,
}

struct Client {
    stream: UnixStream,
    inbuf: Vec<u8>,
    out: VecDeque<Outgoing>,
    queued: usize,
}

impl Client {
    fn send(&mut self, msg: &HolderMsg, fd: Option<OwnedFd>) {
        let bytes = msg.encode();
        self.queued += bytes.len();
        self.out.push_back(Outgoing { bytes, sent: 0, fd });
    }

    /// Returns false when the peer is gone.
    fn flush(&mut self) -> bool {
        while let Some(head) = self.out.front_mut() {
            let mut space = [MaybeUninit::uninit(); rustix::cmsg_space!(ScmRights(1))];
            let mut control = SendAncillaryBuffer::new(&mut space);
            let fds;
            if let Some(fd) = &head.fd {
                fds = [fd.as_fd()];
                control.push(SendAncillaryMessage::ScmRights(&fds));
            }
            let iov = [IoSlice::new(&head.bytes[head.sent..])];
            match sendmsg(
                &self.stream,
                &iov,
                &mut control,
                SendFlags::NOSIGNAL | SendFlags::DONTWAIT,
            ) {
                Ok(n) => {
                    head.fd = None;
                    head.sent += n;
                    self.queued -= n;
                    if head.sent == head.bytes.len() {
                        self.out.pop_front();
                    }
                }
                Err(rustix::io::Errno::AGAIN) => return true,
                Err(rustix::io::Errno::INTR) => {}
                Err(_) => return false,
            }
        }
        true
    }
}

enum Event {
    Listener,
    Client(ClientId, PollFlags),
    Master(SessionId),
    Pidfd(SessionId),
}

pub struct Holder {
    config: Config,
    listener: UnixListener,
    clients: HashMap<ClientId, Client>,
    sessions: BTreeMap<SessionId, Session>,
    next_client: ClientId,
    next_session: SessionId,
    relaunch_at: Option<Instant>,
    relaunches: VecDeque<Instant>,
    idle_since: Option<Instant>,
}

impl Holder {
    pub fn new(listener: UnixListener, config: Config) -> std::io::Result<Self> {
        listener.set_nonblocking(true)?;
        Ok(Holder {
            config,
            listener,
            clients: HashMap::new(),
            sessions: BTreeMap::new(),
            next_client: 1,
            next_session: 1,
            relaunch_at: None,
            relaunches: VecDeque::new(),
            idle_since: Some(Instant::now()),
        })
    }

    /// Runs until the holder was idle for `idle_exit`.
    pub fn run(&mut self) -> std::io::Result<()> {
        loop {
            let now = Instant::now();
            if self.relaunch_at.is_some_and(|at| at <= now) {
                self.relaunch_at = None;
                self.relaunch(now);
            }
            for s in self.sessions.values_mut() {
                if let Owner::Closing { kill_at } = s.owner
                    && kill_at <= now
                {
                    if s.pidfd.is_some() {
                        let _ =
                            rustix::process::kill_process(Pid::from_child(&s.child), Signal::KILL);
                    }
                    s.owner = Owner::Closed;
                }
            }
            let kill_at = self
                .sessions
                .values()
                .filter_map(|s| match s.owner {
                    Owner::Closing { kill_at } => Some(kill_at),
                    _ => None,
                })
                .min();
            let idle_deadline = self
                .idle_since
                .zip(self.config.idle_exit)
                .map(|(since, d)| since + d);
            if idle_deadline.is_some_and(|at| at <= now) {
                return Ok(());
            }
            let deadline = [self.relaunch_at, idle_deadline, kill_at]
                .into_iter()
                .flatten()
                .min();
            let events = self.wait(deadline.map(|at| at.saturating_duration_since(now)))?;
            for event in events {
                self.handle(event);
            }
            self.idle_since = match (
                self.clients.is_empty() && self.sessions.is_empty(),
                self.idle_since,
            ) {
                (true, None) => Some(Instant::now()),
                (true, since) => since,
                (false, _) => None,
            };
        }
    }

    fn paused(&self, s: &Session) -> bool {
        if s.tail.len() >= self.config.tail_cap {
            return true;
        }
        match s.owner {
            Owner::Attached(c) => self
                .clients
                .get(&c)
                .is_some_and(|c| c.queued >= self.config.client_queue_cap),
            Owner::Orphaned | Owner::Closing { .. } | Owner::Closed => false,
        }
    }

    fn wait(&self, timeout: Option<Duration>) -> std::io::Result<Vec<Event>> {
        let mut fds = vec![PollFd::new(&self.listener, PollFlags::IN)];
        let mut kinds = vec![Event::Listener];
        for (id, c) in &self.clients {
            let flags = if c.out.is_empty() {
                PollFlags::IN
            } else {
                PollFlags::IN | PollFlags::OUT
            };
            fds.push(PollFd::new(&c.stream, flags));
            kinds.push(Event::Client(*id, PollFlags::empty()));
        }
        for (id, s) in &self.sessions {
            if let Some(master) = s.master().filter(|_| !self.paused(s)) {
                fds.push(PollFd::new(master, PollFlags::IN));
                kinds.push(Event::Master(*id));
            }
            if let Some(pidfd) = &s.pidfd {
                fds.push(PollFd::new(pidfd, PollFlags::IN));
                kinds.push(Event::Pidfd(*id));
            }
        }
        let timeout = timeout.map(|d| Timespec {
            tv_sec: d.as_secs() as i64,
            tv_nsec: d.subsec_nanos() as i64,
        });
        match poll(&mut fds, timeout.as_ref()) {
            Ok(_) | Err(rustix::io::Errno::INTR) => {}
            Err(e) => return Err(e.into()),
        }
        Ok(fds
            .iter()
            .zip(kinds)
            .filter(|(fd, _)| !fd.revents().is_empty())
            .map(|(fd, kind)| match kind {
                Event::Client(id, _) => Event::Client(id, fd.revents()),
                other => other,
            })
            .collect())
    }

    fn handle(&mut self, event: Event) {
        match event {
            Event::Listener => self.accept(),
            Event::Client(id, revents) => {
                let alive = (!revents.contains(PollFlags::IN) || self.read_client(id))
                    && self.clients.get_mut(&id).is_some_and(|c| c.flush());
                if !alive {
                    self.disconnect(id);
                }
            }
            Event::Master(id) => self.read_master(id),
            Event::Pidfd(id) => self.reap(id),
        }
    }

    fn accept(&mut self) {
        while let Ok((stream, _)) = self.listener.accept() {
            let same_user = rustix::net::sockopt::socket_peercred(&stream)
                .is_ok_and(|cred| cred.uid == rustix::process::getuid());
            if !same_user || stream.set_nonblocking(true).is_err() {
                continue;
            }
            let id = self.next_client;
            self.next_client += 1;
            self.clients.insert(
                id,
                Client {
                    stream,
                    inbuf: Vec::new(),
                    out: VecDeque::new(),
                    queued: 0,
                },
            );
        }
    }

    /// Returns false when the client hung up or broke the protocol.
    fn read_client(&mut self, id: ClientId) -> bool {
        let Some(client) = self.clients.get_mut(&id) else {
            return false;
        };
        let mut buf = [0u8; READ_CHUNK];
        let mut ok = true;
        loop {
            match client.stream.read(&mut buf) {
                Ok(0) => {
                    ok = false;
                    break;
                }
                Ok(n) => client.inbuf.extend_from_slice(&buf[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(_) => return false,
            }
        }
        // Frames that came before a hang-up still count: a client sends its last Snapshot and then exits.
        let inbuf = std::mem::take(&mut client.inbuf);
        let mut pos = 0;
        loop {
            match split_frame(&inbuf[pos..]) {
                Ok(Some((tag, payload, used))) => {
                    pos += used;
                    match ClientMsg::decode(tag, payload) {
                        Ok(msg) => self.on_message(id, msg),
                        Err(_) => {
                            ok = false;
                            break;
                        }
                    }
                }
                Ok(None) => break,
                Err(_) => {
                    ok = false;
                    break;
                }
            }
        }
        if let Some(client) = self.clients.get_mut(&id) {
            client.inbuf = inbuf[pos..].to_vec();
        }
        ok
    }

    fn send(&mut self, client: ClientId, msg: &HolderMsg, fd: Option<OwnedFd>) {
        if let Some(c) = self.clients.get_mut(&client) {
            c.send(msg, fd);
        }
    }

    fn on_message(&mut self, client: ClientId, msg: ClientMsg) {
        match msg {
            ClientMsg::Spawn {
                req,
                size,
                cwd,
                argv,
                env,
            } => match self.spawn(size, &cwd, &argv, &env) {
                Ok((session, fd)) => {
                    let id = self.next_session;
                    self.next_session += 1;
                    self.sessions.insert(
                        id,
                        Session {
                            owner: Owner::Attached(client),
                            ..session
                        },
                    );
                    self.send(client, &HolderMsg::Spawned { req, id }, Some(fd));
                }
                Err(e) => self.send(
                    client,
                    &HolderMsg::SpawnFailed {
                        req,
                        message: e.to_string(),
                    },
                    None,
                ),
            },
            ClientMsg::Attach { id } => self.attach(client, id),
            ClientMsg::Snapshot { id, offset, blob } => {
                if let Some(s) = self
                    .sessions
                    .get_mut(&id)
                    .filter(|s| matches!(s.owner, Owner::Attached(c) if c == client))
                    && offset >= s.snapshot_offset
                    && offset <= s.read_offset()
                {
                    s.tail.drain(..(offset - s.snapshot_offset) as usize);
                    s.snapshot = blob;
                    s.snapshot_offset = offset;
                }
            }
            ClientMsg::Close { id } => {
                if let Some(s) = self
                    .sessions
                    .get_mut(&id)
                    .filter(|s| matches!(s.owner, Owner::Attached(c) if c == client))
                {
                    s.owner = Owner::Closed;
                    if s.pidfd.is_some() {
                        let _ =
                            rustix::process::kill_process(Pid::from_child(&s.child), Signal::HUP);
                        s.owner = Owner::Closing {
                            kill_at: Instant::now() + self.config.close_grace,
                        };
                    }
                    s.on_hangup();
                    s.tail = Vec::new();
                    s.snapshot = Vec::new();
                    self.drop_if_done(id);
                }
            }
            ClientMsg::List => {
                let ids = self
                    .sessions
                    .iter()
                    .filter(|(_, s)| matches!(s.owner, Owner::Orphaned))
                    .map(|(id, _)| *id)
                    .collect();
                self.send(client, &HolderMsg::Orphans { ids }, None);
            }
        }
    }

    fn spawn(
        &self,
        size: WinSize,
        cwd: &str,
        argv: &[String],
        env: &[String],
    ) -> std::io::Result<(Session, OwnedFd)> {
        let program = argv.first().ok_or(std::io::ErrorKind::InvalidInput)?;
        let master =
            rustix::pty::openpt(rustix::pty::OpenptFlags::RDWR | rustix::pty::OpenptFlags::NOCTTY)?;
        rustix::pty::grantpt(&master)?;
        rustix::pty::unlockpt(&master)?;
        let name = rustix::pty::ptsname(&master, Vec::new())?;
        let slave = rustix::fs::open(
            name.as_c_str(),
            OFlags::RDWR | OFlags::NOCTTY | OFlags::CLOEXEC,
            Mode::empty(),
        )?;
        rustix::termios::tcsetwinsize(
            &slave,
            rustix::termios::Winsize {
                ws_row: size.rows,
                ws_col: size.cols,
                ws_xpixel: size.xpixel,
                ws_ypixel: size.ypixel,
            },
        )?;
        rustix::io::fcntl_setfd(&master, rustix::io::FdFlags::CLOEXEC)?;
        rustix::fs::fcntl_setfl(&master, OFlags::NONBLOCK)?;

        let mut cmd = Command::new(program);
        cmd.args(&argv[1..])
            .env_clear()
            .envs(
                env.iter()
                    .filter_map(|kv| kv.split_once('='))
                    .map(|(k, v)| (OsString::from(k), OsString::from_vec(v.into()))),
            )
            .stdin(Stdio::from(slave.try_clone()?))
            .stdout(Stdio::from(slave.try_clone()?))
            .stderr(Stdio::from(slave));
        if !cwd.is_empty() {
            cmd.current_dir(cwd);
        }
        // SAFETY: setsid and ioctl are async-signal-safe and touch no memory of the parent.
        #[allow(unsafe_code)]
        unsafe {
            cmd.pre_exec(|| {
                rustix::process::setsid()?;
                rustix::process::ioctl_tiocsctty(std::os::fd::BorrowedFd::borrow_raw(0))?;
                Ok(())
            });
        }
        let child = cmd.spawn()?;
        let pidfd = rustix::process::pidfd_open(Pid::from_child(&child), PidfdFlags::empty())?;
        let client_fd = rustix::io::fcntl_dupfd_cloexec(&master, 0)?;
        Ok((
            Session {
                child,
                pidfd: Some(pidfd),
                life: Life::Running { master },
                owner: Owner::Orphaned,
                snapshot: Vec::new(),
                snapshot_offset: 0,
                tail: Vec::new(),
                want_sent_at: 0,
            },
            client_fd,
        ))
    }

    fn attach(&mut self, client: ClientId, id: SessionId) {
        let Some(s) = self
            .sessions
            .get_mut(&id)
            .filter(|s| matches!(s.owner, Owner::Orphaned))
        else {
            self.send(client, &HolderMsg::AttachFailed { id }, None);
            return;
        };
        let fd = s
            .master()
            .map(|m| rustix::io::fcntl_dupfd_cloexec(m, 0))
            .transpose()
            .ok()
            .flatten();
        s.owner = Owner::Attached(client);
        let attached = HolderMsg::Attached {
            id,
            offset: s.snapshot_offset,
            snapshot: s.snapshot.clone(),
            tail: s.tail.clone(),
        };
        let exited = match s.life {
            Life::Exited { status } => Some(HolderMsg::Exited { id, status }),
            _ => None,
        };
        self.send(client, &attached, fd);
        if let Some(exited) = exited {
            self.send(client, &exited, None);
        }
        self.maybe_want_snapshot(id);
    }

    fn maybe_want_snapshot(&mut self, id: SessionId) {
        let Some(s) = self.sessions.get_mut(&id) else {
            return;
        };
        let Owner::Attached(client) = s.owner else {
            return;
        };
        let threshold = self.config.snapshot_at as u64;
        if s.tail.len() as u64 >= threshold && s.read_offset() >= s.want_sent_at + threshold {
            s.want_sent_at = s.read_offset();
            self.send(client, &HolderMsg::WantSnapshot { id }, None);
        }
    }

    fn read_master(&mut self, id: SessionId) {
        let cap = self.config.tail_cap;
        let Some(s) = self.sessions.get_mut(&id) else {
            return;
        };
        let Some(master) = s.master() else { return };
        let mut buf = vec![0u8; READ_CHUNK.min(cap.saturating_sub(s.tail.len())).max(1)];
        match rustix::io::read(master, &mut buf) {
            Ok(0) | Err(rustix::io::Errno::IO) => {
                s.on_hangup();
                self.finish_if_exited(id);
            }
            Ok(n) => {
                buf.truncate(n);
                let offset = s.read_offset();
                s.tail.extend_from_slice(&buf);
                if let Owner::Attached(client) = s.owner {
                    self.send(
                        client,
                        &HolderMsg::Output {
                            id,
                            offset,
                            bytes: buf,
                        },
                        None,
                    );
                    if let Some(c) = self.clients.get_mut(&client)
                        && !c.flush()
                    {
                        self.disconnect(client);
                    }
                }
                self.maybe_want_snapshot(id);
            }
            Err(_) => {}
        }
    }

    fn reap(&mut self, id: SessionId) {
        let Some(s) = self.sessions.get_mut(&id) else {
            return;
        };
        if let Ok(Some(status)) = s.child.try_wait() {
            use std::os::unix::process::ExitStatusExt;
            s.on_reaped(status.into_raw());
            self.finish_if_exited(id);
        }
    }

    fn finish_if_exited(&mut self, id: SessionId) {
        let Some(s) = self.sessions.get(&id) else {
            return;
        };
        let Life::Exited { status } = s.life else {
            return;
        };
        match s.owner {
            Owner::Attached(client) => self.send(client, &HolderMsg::Exited { id, status }, None),
            Owner::Orphaned | Owner::Closing { .. } | Owner::Closed => {
                self.sessions.remove(&id);
            }
        }
    }

    fn drop_if_done(&mut self, id: SessionId) {
        if self
            .sessions
            .get(&id)
            .is_some_and(|s| matches!(s.life, Life::Exited { .. }))
        {
            self.sessions.remove(&id);
        }
    }

    fn disconnect(&mut self, client: ClientId) {
        self.clients.remove(&client);
        let mut orphaned_running = false;
        let mut exited = Vec::new();
        for (id, s) in &mut self.sessions {
            if matches!(s.owner, Owner::Attached(c) if c == client) {
                s.owner = Owner::Orphaned;
                match s.life {
                    Life::Exited { .. } => exited.push(*id),
                    _ => orphaned_running = true,
                }
            }
        }
        for id in exited {
            self.sessions.remove(&id);
        }
        if orphaned_running && self.config.relaunch.is_some() && self.relaunch_at.is_none() {
            self.relaunch_at = Some(Instant::now() + self.config.relaunch_delay);
        }
    }

    fn relaunch(&mut self, now: Instant) {
        while self
            .relaunches
            .front()
            .is_some_and(|t| now.duration_since(*t) > RELAUNCH_WINDOW)
        {
            self.relaunches.pop_front();
        }
        let any_orphan = self
            .sessions
            .values()
            .any(|s| matches!(s.owner, Owner::Orphaned));
        let Some(argv) = self.config.relaunch.as_ref().filter(|_| any_orphan) else {
            return;
        };
        if self.relaunches.len() >= RELAUNCH_LIMIT {
            eprintln!(
                "andromeda-ptyd: {RELAUNCH_LIMIT} relaunches in a minute; orphaned sessions wait for a terminal"
            );
            return;
        }
        self.relaunches.push_back(now);
        match Command::new(&argv[0])
            .args(&argv[1..])
            .stdin(Stdio::null())
            .spawn()
        {
            Ok(mut child) => {
                std::thread::spawn(move || child.wait());
            }
            Err(e) => eprintln!("andromeda-ptyd: relaunch {:?} failed: {e}", argv[0]),
        }
    }
}
