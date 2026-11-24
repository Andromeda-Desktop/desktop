//! One test per file: the sandbox confines the whole test process.

#![allow(clippy::unwrap_used)]

use andromeda_sandbox::{Profile, enter};
use rustix::fs::MemfdFlags;
use rustix::io::Errno;
use rustix::net::{AddressFamily, SocketType};
use std::io::Write;

#[test]
fn extract_reads_any_file_but_cannot_write_connect_exec_or_make_fds() {
    let mut doc = tempfile::NamedTempFile::new_in(std::env::var("HOME").unwrap()).unwrap();
    doc.write_all(b"words").unwrap();

    enter(Profile::Extract).unwrap();

    assert_eq!(std::fs::read(doc.path()).unwrap(), b"words");
    assert!(doc.path().metadata().is_ok());
    assert!(std::fs::OpenOptions::new().append(true).open(doc.path()).is_err());
    assert!(std::fs::write(std::env::temp_dir().join("andromeda-sandbox-probe"), b"x").is_err());
    assert_eq!(rustix::net::socket(AddressFamily::UNIX, SocketType::STREAM, None).unwrap_err(), Errno::PERM);
    assert_eq!(rustix::net::socket(AddressFamily::INET, SocketType::STREAM, None).unwrap_err(), Errno::PERM);
    assert!(std::process::Command::new("/usr/bin/true").status().is_err());
    assert_eq!(rustix::fs::memfd_create("probe", MemfdFlags::CLOEXEC).unwrap_err(), Errno::PERM);
}
