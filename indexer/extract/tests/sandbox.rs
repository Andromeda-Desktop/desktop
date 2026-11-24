//! The extractor binary answers its first request from inside the Extract sandbox.

use std::io::{BufRead, BufReader, Write};
use std::process::{Command, Stdio};

#[test]
fn extractor_answers_from_inside_its_sandbox() {
    let dir = tempfile::tempdir().unwrap();
    let page = dir.path().join("page.html");
    std::fs::write(&page, "<p>okapi</p>").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_andromeda-index-extract"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    writeln!(stdin, "html {}", page.display()).unwrap();
    let mut reply = BufReader::new(child.stdout.take().unwrap());
    let mut header = String::new();
    reply.read_line(&mut header).unwrap();
    assert_eq!(header, "6\n");

    let status = std::fs::read_to_string(format!("/proc/{}/status", child.id())).unwrap();
    assert!(status.lines().any(|l| l == "Seccomp:\t2"), "no seccomp filter:\n{status}");
    assert!(status.lines().any(|l| l == "NoNewPrivs:\t1"), "no-new-privs not set:\n{status}");
    drop(stdin);
    assert!(child.wait().unwrap().success());
}
