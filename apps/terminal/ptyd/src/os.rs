//! The OS layer: the holder reaches Linux- or macOS-only calls only through these.

use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::process::Child;

pub use imp::*;

#[cfg(target_os = "linux")]
mod imp {
    use super::*;
    use rustix::process::{Pid, PidfdFlags};

    /// Sends never raise SIGPIPE: MSG_NOSIGNAL on each send.
    pub const SEND_FLAGS: rustix::net::SendFlags = rustix::net::SendFlags::NOSIGNAL;
    /// The highest signal number, real-time signals included.
    pub fn max_signal() -> i32 {
        libc::SIGRTMAX()
    }

    /// An fd that polls readable once the child exited: its pidfd.
    pub fn exit_watch(child: &Child) -> std::io::Result<OwnedFd> {
        Ok(rustix::process::pidfd_open(Pid::from_child(child), PidfdFlags::empty())?)
    }

    pub fn prepare_stream(_stream: &UnixStream) -> std::io::Result<()> {
        Ok(())
    }

    pub fn peer_uid(stream: &UnixStream) -> Option<u32> {
        rustix::net::sockopt::socket_peercred(stream.as_fd())
            .ok()
            .map(|cred| cred.uid.as_raw())
    }

    pub fn process_name(pid: i32) -> Option<String> {
        std::fs::read_to_string(format!("/proc/{pid}/comm"))
            .ok()
            .map(|c| c.trim_end().to_owned())
    }

    pub fn process_cwd(pid: u32) -> Option<String> {
        std::fs::read_link(format!("/proc/{pid}/cwd"))
            .ok()
            .map(|p| p.to_string_lossy().into_owned())
    }

    /// Every process of session sid except its leader.
    pub fn session_processes(sid: u32) -> Vec<i32> {
        let Ok(dir) = std::fs::read_dir("/proc") else {
            return Vec::new();
        };
        let mut out = Vec::new();
        for entry in dir.flatten() {
            let Some(pid) = entry
                .file_name()
                .to_str()
                .and_then(|n| n.parse::<u32>().ok())
            else {
                continue;
            };
            if pid == sid {
                continue;
            }
            let Ok(stat) = std::fs::read_to_string(format!("/proc/{pid}/stat")) else {
                continue;
            };
            // The name may hold spaces and parentheses: fields restart after the last ')'.
            let session = stat
                .rsplit_once(')')
                .and_then(|(_, rest)| rest.split_whitespace().nth(3))
                .and_then(|f| f.parse::<u32>().ok());
            if session == Some(sid)
                && let Ok(pid) = i32::try_from(pid)
            {
                out.push(pid);
            }
        }
        out
    }
}

#[cfg(target_os = "macos")]
#[allow(unsafe_code)]
mod imp {
    use super::*;
    use std::os::fd::{AsRawFd, FromRawFd};

    /// Streams carry SO_NOSIGPIPE (prepare_stream), so sends need no flag.
    pub const SEND_FLAGS: rustix::net::SendFlags = rustix::net::SendFlags::empty();
    /// SIGUSR2 (31) is the last signal; macOS has no real-time signals.
    pub fn max_signal() -> i32 {
        libc::SIGUSR2
    }

    /// An fd that polls readable once the child exited: a kqueue with EVFILT_PROC NOTE_EXIT on the pid. A child that
    /// already exited gets a triggered EVFILT_USER instead.
    pub fn exit_watch(child: &Child) -> std::io::Result<OwnedFd> {
        // SAFETY: kqueue returns a new fd or -1; ownership passes to OwnedFd at once.
        let raw = unsafe { libc::kqueue() };
        if raw < 0 {
            return Err(std::io::Error::last_os_error());
        }
        // SAFETY: raw is a new fd that nothing else owns.
        let kq = unsafe { OwnedFd::from_raw_fd(raw) };
        rustix::io::fcntl_setfd(&kq, rustix::io::FdFlags::CLOEXEC)?;
        let register = |filter: i16, ident: usize, fflags: u32| {
            let change = libc::kevent {
                ident,
                filter,
                flags: libc::EV_ADD | libc::EV_ONESHOT,
                fflags,
                data: 0,
                udata: std::ptr::null_mut(),
            };
            // SAFETY: one valid change, no event buffer.
            let rc = unsafe {
                libc::kevent(kq.as_raw_fd(), &change, 1, std::ptr::null_mut(), 0, std::ptr::null())
            };
            if rc < 0 { Err(std::io::Error::last_os_error()) } else { Ok(()) }
        };
        match register(libc::EVFILT_PROC, child.id() as usize, libc::NOTE_EXIT) {
            Err(e) if e.raw_os_error() == Some(libc::ESRCH) => {
                register(libc::EVFILT_USER, 0, libc::NOTE_TRIGGER)?
            }
            other => other?,
        }
        Ok(kq)
    }

    pub fn prepare_stream(stream: &UnixStream) -> std::io::Result<()> {
        rustix::net::sockopt::set_socket_nosigpipe(stream.as_fd(), true)?;
        Ok(())
    }

    pub fn peer_uid(stream: &UnixStream) -> Option<u32> {
        let mut uid: libc::uid_t = 0;
        let mut gid: libc::gid_t = 0;
        // SAFETY: the fd is a connected Unix socket; uid and gid are valid out pointers.
        let rc = unsafe { libc::getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) };
        (rc == 0).then_some(uid)
    }

    pub fn process_name(pid: i32) -> Option<String> {
        let mut buf = [0u8; 2 * libc::MAXCOMLEN + 1];
        // SAFETY: buf is valid for its length.
        let n = unsafe { libc::proc_name(pid, buf.as_mut_ptr().cast(), buf.len() as u32) };
        (n > 0).then(|| String::from_utf8_lossy(&buf[..n as usize]).into_owned())
    }

    pub fn process_cwd(pid: u32) -> Option<String> {
        let mut info = std::mem::MaybeUninit::<libc::proc_vnodepathinfo>::zeroed();
        let size = std::mem::size_of::<libc::proc_vnodepathinfo>() as i32;
        // SAFETY: info is valid for size bytes; proc_pidinfo writes at most that.
        let n = unsafe {
            libc::proc_pidinfo(pid as i32, libc::PROC_PIDVNODEPATHINFO, 0, info.as_mut_ptr().cast(), size)
        };
        if n != size {
            return None;
        }
        // SAFETY: proc_pidinfo filled the whole struct.
        let info = unsafe { info.assume_init() };
        let path = info.pvi_cdir.vip_path;
        // SAFETY: vip_path is a NUL-terminated C string inside the struct.
        let cstr = unsafe { std::ffi::CStr::from_ptr(path.as_ptr().cast()) };
        Some(cstr.to_string_lossy().into_owned()).filter(|p| !p.is_empty())
    }

    /// Every process of session sid except its leader.
    pub fn session_processes(sid: u32) -> Vec<i32> {
        // SAFETY: a count query: no buffer.
        let count = unsafe { libc::proc_listallpids(std::ptr::null_mut(), 0) };
        if count <= 0 {
            return Vec::new();
        }
        let mut pids = vec![0 as libc::pid_t; count as usize + 32];
        let bytes = (pids.len() * std::mem::size_of::<libc::pid_t>()) as i32;
        // SAFETY: pids is valid for bytes; the call returns how many pids it wrote.
        let n = unsafe { libc::proc_listallpids(pids.as_mut_ptr().cast(), bytes) };
        pids.truncate(n.max(0) as usize);
        pids.into_iter()
            // SAFETY: getsid only reads the pid.
            .filter(|&pid| pid > 0 && pid as u32 != sid && unsafe { libc::getsid(pid) } == sid as i32)
            .collect()
    }
}
