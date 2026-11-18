//! Wire format between andromeda-terminal and andromeda-ptyd.
//!
//! Frame: u32 length (of type byte + payload), u8 type, payload. Integers are little-endian; a string or
//! byte blob is a u32 length and its bytes; a list is a u32 count and its items. `Spawned` and `Attached`
//! carry a dup of the pty master in SCM_RIGHTS: the client writes input and sets the window size on it,
//! and never reads it. A new incompatible format gets a new socket name (`ptyd-<version>.sock`).

pub const MAX_FRAME: usize = 64 << 20;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct WinSize {
    pub cols: u16,
    pub rows: u16,
    pub xpixel: u16,
    pub ypixel: u16,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClientMsg {
    Spawn {
        req: u32,
        size: WinSize,
        cwd: String,
        argv: Vec<String>,
        env: Vec<String>,
    },
    Attach {
        id: u64,
    },
    /// `blob` is the client's terminal state after it parsed every output byte before `offset`.
    Snapshot {
        id: u64,
        offset: u64,
        blob: Vec<u8>,
    },
    /// The user closed the session: the holder hangs up its child.
    Close {
        id: u64,
    },
    List,
    /// Subscribes this connection to `Workspaces`: one now, then one after each change.
    Watch,
    /// Creates (`id` 0) or edits a workspace; the holder answers `WorkspacePut`.
    PutWorkspace {
        req: u32,
        id: u64,
        name: String,
        root: String,
        symbol: String,
    },
    /// Hangs up every session in the workspace and forgets it.
    CloseWorkspace {
        id: u64,
    },
    /// Moves session `id` into `workspace` (0: none).
    SetWorkspace {
        id: u64,
        workspace: u64,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceInfo {
    pub id: u64,
    pub name: String,
    pub root: String,
    pub symbol: String,
}

/// The last command of a session, from the shell's OSC 133 marks.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LastCommand {
    #[default]
    None,
    Running,
    Done(i32),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionInfo {
    pub id: u64,
    pub workspace: u64,
    pub attached: bool,
    /// The foreground process name (`/proc/<pgid>/comm`); empty until first seen.
    pub program: String,
    pub cwd: String,
    pub command: LastCommand,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HolderMsg {
    Spawned {
        req: u32,
        id: u64,
    },
    SpawnFailed {
        req: u32,
        message: String,
    },
    /// Replay: restore `snapshot` (empty: a fresh terminal), then parse `tail`, which starts at `offset`.
    Attached {
        id: u64,
        offset: u64,
        snapshot: Vec<u8>,
        tail: Vec<u8>,
    },
    AttachFailed {
        id: u64,
    },
    Output {
        id: u64,
        offset: u64,
        bytes: Vec<u8>,
    },
    WantSnapshot {
        id: u64,
    },
    /// `status` is the raw wait status.
    Exited {
        id: u64,
        status: i32,
    },
    Orphans {
        ids: Vec<u64>,
    },
    WorkspacePut {
        req: u32,
        id: u64,
    },
    /// The whole table; `seq` grows by one per message, so a client drops an older one.
    Workspaces {
        seq: u64,
        workspaces: Vec<WorkspaceInfo>,
        sessions: Vec<SessionInfo>,
    },
}

mod tag {
    pub const SPAWN: u8 = 1;
    pub const ATTACH: u8 = 2;
    pub const SNAPSHOT: u8 = 3;
    pub const CLOSE: u8 = 4;
    pub const LIST: u8 = 5;
    pub const WATCH: u8 = 6;
    pub const PUT_WORKSPACE: u8 = 7;
    pub const CLOSE_WORKSPACE: u8 = 8;
    pub const SET_WORKSPACE: u8 = 9;
    pub const SPAWNED: u8 = 10;
    pub const SPAWN_FAILED: u8 = 11;
    pub const ATTACHED: u8 = 12;
    pub const ATTACH_FAILED: u8 = 13;
    pub const OUTPUT: u8 = 14;
    pub const WANT_SNAPSHOT: u8 = 15;
    pub const EXITED: u8 = 16;
    pub const ORPHANS: u8 = 17;
    pub const WORKSPACE_PUT: u8 = 18;
    pub const WORKSPACES: u8 = 19;
}

#[derive(Debug, PartialEq, Eq)]
pub enum DecodeError {
    Truncated,
    BadTag(u8),
    BadUtf8,
    TooLarge(usize),
}

struct Writer(Vec<u8>);

impl Writer {
    fn new(tag: u8) -> Self {
        let mut v = Vec::with_capacity(64);
        v.extend_from_slice(&[0, 0, 0, 0, tag]);
        Writer(v)
    }
    fn u8(&mut self, v: u8) -> &mut Self {
        self.0.push(v);
        self
    }
    fn str(&mut self, v: &str) -> &mut Self {
        self.bytes(v.as_bytes())
    }
    fn u16(&mut self, v: u16) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u32(&mut self, v: u32) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u64(&mut self, v: u64) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn bytes(&mut self, v: &[u8]) -> &mut Self {
        self.u32(v.len() as u32);
        self.0.extend_from_slice(v);
        self
    }
    fn strs(&mut self, v: &[String]) -> &mut Self {
        self.u32(v.len() as u32);
        for s in v {
            self.bytes(s.as_bytes());
        }
        self
    }
    fn finish(&mut self) -> Vec<u8> {
        let mut v = std::mem::take(&mut self.0);
        let len = (v.len() - 4) as u32;
        v[..4].copy_from_slice(&len.to_le_bytes());
        v
    }
}

struct Reader<'a>(&'a [u8]);

impl Reader<'_> {
    fn take(&mut self, n: usize) -> Result<&[u8], DecodeError> {
        if self.0.len() < n {
            return Err(DecodeError::Truncated);
        }
        let (head, rest) = self.0.split_at(n);
        self.0 = rest;
        Ok(head)
    }
    fn u8(&mut self) -> Result<u8, DecodeError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, DecodeError> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32, DecodeError> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64, DecodeError> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn bytes(&mut self) -> Result<Vec<u8>, DecodeError> {
        let n = self.u32()? as usize;
        Ok(self.take(n)?.to_vec())
    }
    fn str(&mut self) -> Result<String, DecodeError> {
        String::from_utf8(self.bytes()?).map_err(|_| DecodeError::BadUtf8)
    }
    fn strs(&mut self) -> Result<Vec<String>, DecodeError> {
        let n = self.u32()? as usize;
        (0..n).map(|_| self.str()).collect()
    }
    fn size(&mut self) -> Result<WinSize, DecodeError> {
        Ok(WinSize {
            cols: self.u16()?,
            rows: self.u16()?,
            xpixel: self.u16()?,
            ypixel: self.u16()?,
        })
    }
}

/// Splits one frame off the front of `buf`: `Ok(None)` when the frame is not complete yet.
/// Type byte, payload, and bytes used from the buffer.
pub type Frame<'a> = (u8, &'a [u8], usize);

pub fn split_frame(buf: &[u8]) -> Result<Option<Frame<'_>>, DecodeError> {
    if buf.len() < 4 {
        return Ok(None);
    }
    let len = u32::from_le_bytes(buf[..4].try_into().unwrap()) as usize;
    if len == 0 {
        return Err(DecodeError::Truncated);
    }
    if len > MAX_FRAME {
        return Err(DecodeError::TooLarge(len));
    }
    if buf.len() < 4 + len {
        return Ok(None);
    }
    Ok(Some((buf[4], &buf[5..4 + len], 4 + len)))
}

impl ClientMsg {
    pub fn encode(&self) -> Vec<u8> {
        match self {
            ClientMsg::Spawn {
                req,
                size,
                cwd,
                argv,
                env,
            } => Writer::new(tag::SPAWN)
                .u32(*req)
                .u16(size.cols)
                .u16(size.rows)
                .u16(size.xpixel)
                .u16(size.ypixel)
                .bytes(cwd.as_bytes())
                .strs(argv)
                .strs(env)
                .finish(),
            ClientMsg::Attach { id } => Writer::new(tag::ATTACH).u64(*id).finish(),
            ClientMsg::Snapshot { id, offset, blob } => Writer::new(tag::SNAPSHOT)
                .u64(*id)
                .u64(*offset)
                .bytes(blob)
                .finish(),
            ClientMsg::Close { id } => Writer::new(tag::CLOSE).u64(*id).finish(),
            ClientMsg::List => Writer::new(tag::LIST).finish(),
            ClientMsg::Watch => Writer::new(tag::WATCH).finish(),
            ClientMsg::PutWorkspace {
                req,
                id,
                name,
                root,
                symbol,
            } => Writer::new(tag::PUT_WORKSPACE)
                .u32(*req)
                .u64(*id)
                .str(name)
                .str(root)
                .str(symbol)
                .finish(),
            ClientMsg::CloseWorkspace { id } => Writer::new(tag::CLOSE_WORKSPACE).u64(*id).finish(),
            ClientMsg::SetWorkspace { id, workspace } => Writer::new(tag::SET_WORKSPACE)
                .u64(*id)
                .u64(*workspace)
                .finish(),
        }
    }

    pub fn decode(tag: u8, payload: &[u8]) -> Result<Self, DecodeError> {
        let r = &mut Reader(payload);
        Ok(match tag {
            tag::SPAWN => ClientMsg::Spawn {
                req: r.u32()?,
                size: r.size()?,
                cwd: r.str()?,
                argv: r.strs()?,
                env: r.strs()?,
            },
            tag::ATTACH => ClientMsg::Attach { id: r.u64()? },
            tag::SNAPSHOT => ClientMsg::Snapshot {
                id: r.u64()?,
                offset: r.u64()?,
                blob: r.bytes()?,
            },
            tag::CLOSE => ClientMsg::Close { id: r.u64()? },
            tag::LIST => ClientMsg::List,
            tag::WATCH => ClientMsg::Watch,
            tag::PUT_WORKSPACE => ClientMsg::PutWorkspace {
                req: r.u32()?,
                id: r.u64()?,
                name: r.str()?,
                root: r.str()?,
                symbol: r.str()?,
            },
            tag::CLOSE_WORKSPACE => ClientMsg::CloseWorkspace { id: r.u64()? },
            tag::SET_WORKSPACE => ClientMsg::SetWorkspace {
                id: r.u64()?,
                workspace: r.u64()?,
            },
            t => return Err(DecodeError::BadTag(t)),
        })
    }
}

impl HolderMsg {
    pub fn encode(&self) -> Vec<u8> {
        match self {
            HolderMsg::Spawned { req, id } => Writer::new(tag::SPAWNED).u32(*req).u64(*id).finish(),
            HolderMsg::SpawnFailed { req, message } => Writer::new(tag::SPAWN_FAILED)
                .u32(*req)
                .bytes(message.as_bytes())
                .finish(),
            HolderMsg::Attached {
                id,
                offset,
                snapshot,
                tail,
            } => Writer::new(tag::ATTACHED)
                .u64(*id)
                .u64(*offset)
                .bytes(snapshot)
                .bytes(tail)
                .finish(),
            HolderMsg::AttachFailed { id } => Writer::new(tag::ATTACH_FAILED).u64(*id).finish(),
            HolderMsg::Output { id, offset, bytes } => Writer::new(tag::OUTPUT)
                .u64(*id)
                .u64(*offset)
                .bytes(bytes)
                .finish(),
            HolderMsg::WantSnapshot { id } => Writer::new(tag::WANT_SNAPSHOT).u64(*id).finish(),
            HolderMsg::Exited { id, status } => Writer::new(tag::EXITED)
                .u64(*id)
                .u32(*status as u32)
                .finish(),
            HolderMsg::Orphans { ids } => {
                let mut w = Writer::new(tag::ORPHANS);
                w.u32(ids.len() as u32);
                for id in ids {
                    w.u64(*id);
                }
                w.finish()
            }
            HolderMsg::WorkspacePut { req, id } => {
                Writer::new(tag::WORKSPACE_PUT).u32(*req).u64(*id).finish()
            }
            HolderMsg::Workspaces {
                seq,
                workspaces,
                sessions,
            } => {
                let mut w = Writer::new(tag::WORKSPACES);
                w.u64(*seq).u32(workspaces.len() as u32);
                for ws in workspaces {
                    w.u64(ws.id).str(&ws.name).str(&ws.root).str(&ws.symbol);
                }
                w.u32(sessions.len() as u32);
                for s in sessions {
                    let (kind, code) = match s.command {
                        LastCommand::None => (0, 0),
                        LastCommand::Running => (1, 0),
                        LastCommand::Done(code) => (2, code),
                    };
                    w.u64(s.id)
                        .u64(s.workspace)
                        .u8(u8::from(s.attached))
                        .str(&s.program)
                        .str(&s.cwd)
                        .u8(kind)
                        .u32(code as u32);
                }
                w.finish()
            }
        }
    }

    pub fn decode(tag: u8, payload: &[u8]) -> Result<Self, DecodeError> {
        let r = &mut Reader(payload);
        Ok(match tag {
            tag::SPAWNED => HolderMsg::Spawned {
                req: r.u32()?,
                id: r.u64()?,
            },
            tag::SPAWN_FAILED => HolderMsg::SpawnFailed {
                req: r.u32()?,
                message: r.str()?,
            },
            tag::ATTACHED => HolderMsg::Attached {
                id: r.u64()?,
                offset: r.u64()?,
                snapshot: r.bytes()?,
                tail: r.bytes()?,
            },
            tag::ATTACH_FAILED => HolderMsg::AttachFailed { id: r.u64()? },
            tag::OUTPUT => HolderMsg::Output {
                id: r.u64()?,
                offset: r.u64()?,
                bytes: r.bytes()?,
            },
            tag::WANT_SNAPSHOT => HolderMsg::WantSnapshot { id: r.u64()? },
            tag::EXITED => HolderMsg::Exited {
                id: r.u64()?,
                status: r.u32()? as i32,
            },
            tag::ORPHANS => {
                let n = r.u32()? as usize;
                HolderMsg::Orphans {
                    ids: (0..n).map(|_| r.u64()).collect::<Result<_, _>>()?,
                }
            }
            tag::WORKSPACE_PUT => HolderMsg::WorkspacePut {
                req: r.u32()?,
                id: r.u64()?,
            },
            tag::WORKSPACES => {
                let seq = r.u64()?;
                let n = r.u32()? as usize;
                let workspaces = (0..n)
                    .map(|_| {
                        Ok(WorkspaceInfo {
                            id: r.u64()?,
                            name: r.str()?,
                            root: r.str()?,
                            symbol: r.str()?,
                        })
                    })
                    .collect::<Result<_, DecodeError>>()?;
                let n = r.u32()? as usize;
                let sessions = (0..n)
                    .map(|_| {
                        let id = r.u64()?;
                        let workspace = r.u64()?;
                        let attached = r.u8()? != 0;
                        let program = r.str()?;
                        let cwd = r.str()?;
                        let command = match (r.u8()?, r.u32()? as i32) {
                            (0, _) => LastCommand::None,
                            (1, _) => LastCommand::Running,
                            (2, code) => LastCommand::Done(code),
                            (t, _) => return Err(DecodeError::BadTag(t)),
                        };
                        Ok(SessionInfo {
                            id,
                            workspace,
                            attached,
                            program,
                            cwd,
                            command,
                        })
                    })
                    .collect::<Result<_, DecodeError>>()?;
                HolderMsg::Workspaces {
                    seq,
                    workspaces,
                    sessions,
                }
            }
            t => return Err(DecodeError::BadTag(t)),
        })
    }
}
