//! `serve`: the root side of a Files administrator tab. It prints `{"ready":true}`, then reads one JSON request per
//! line on stdin and writes one JSON reply per line on stdout, in order. It ends at stdin EOF (the tab or Files
//! closed). Every path passes `checked` (listing may also read `/`), and every step is relative to a folder opened
//! with `O_NOFOLLOW` from `/`, so no link is followed.
//!
//! Request: `{"id":1,"op":"list","path":"/etc"}`; ops: list, mkdir {path}, rename {path,name}, copy and move
//! {sources,dest}, delete {paths}, chmod {paths,mode,mask,recursive}, chown {paths,uid?,gid?,recursive}.
//! Reply: `{"id":1,"ok":true,"entries":[...]}` (list), `{"id":1,"ok":true,"count":n}`, or `{"id":1,"ok":false,"error":"..."}`.

use std::ffi::{OsStr, OsString};
use std::fs::File;
use std::io::{self, BufRead, Write};
use std::os::fd::OwnedFd;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

use rustix::fs::{
    AtFlags, Dir, FileType, Mode, OFlags, RenameFlags, Stat, fchmod, fstat, mkdirat, openat, readlinkat, renameat_with,
    statat, symlinkat, unlinkat,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use crate::{Change, apply, checked, open_parent};

#[derive(Deserialize)]
struct Request {
    id: u64,
    #[serde(flatten)]
    op: Op,
}

#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "lowercase")]
enum Op {
    List { path: String },
    Mkdir { path: String },
    Rename { path: String, name: String },
    Copy { sources: Vec<String>, dest: String },
    Move { sources: Vec<String>, dest: String },
    Delete { paths: Vec<String> },
    Chmod { paths: Vec<String>, mode: u32, mask: u32, recursive: bool },
    Chown { paths: Vec<String>, uid: Option<u32>, gid: Option<u32>, recursive: bool },
}

#[derive(Serialize, Debug, PartialEq)]
pub struct Entry {
    name: String,
    kind: &'static str,
    size: u64,
    mtime_ns: i64,
    mode: u32,
    uid: u32,
    gid: u32,
    /// The name is not UTF-8: `name` is a lossy copy, and a change to it would miss the file.
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    lossy: bool,
}

fn err_at(path: &Path) -> impl Fn(rustix::io::Errno) -> String + '_ {
    move |e| format!("{}: {e}", path.display())
}

const DIR_FLAGS: OFlags = OFlags::DIRECTORY.union(OFlags::NOFOLLOW).union(OFlags::RDONLY).union(OFlags::CLOEXEC);

/// The folder at `path` itself, opened without following a link anywhere on the path.
fn open_dir(path: &Path) -> Result<OwnedFd, String> {
    if path == Path::new("/") {
        return openat(rustix::fs::CWD, "/", DIR_FLAGS, Mode::empty()).map_err(err_at(path));
    }
    let (parent, name) = open_parent(path)?;
    openat(&parent, &name, DIR_FLAGS, Mode::empty()).map_err(err_at(path))
}

fn kind_of(st: &Stat) -> FileType {
    FileType::from_raw_mode(st.st_mode)
}

/// Opens the folder `name` in `dir` and checks it is still the inode `st` describes.
fn enter(dir: &OwnedFd, name: &OsStr, st: &Stat, shown: &Path) -> Result<OwnedFd, String> {
    let child = openat(dir, name, DIR_FLAGS, Mode::empty()).map_err(err_at(shown))?;
    let now = fstat(&child).map_err(err_at(shown))?;
    if now.st_ino != st.st_ino || now.st_dev != st.st_dev {
        return Err(format!("{}: changed while it was being changed", shown.display()));
    }
    Ok(child)
}

fn names(dir: &OwnedFd, shown: &Path) -> Result<Vec<OsString>, String> {
    let mut out = Vec::new();
    for entry in Dir::read_from(dir).map_err(err_at(shown))? {
        let entry = entry.map_err(err_at(shown))?;
        let bytes = entry.file_name().to_bytes();
        if bytes != b"." && bytes != b".." {
            out.push(OsStr::from_bytes(bytes).to_owned());
        }
    }
    Ok(out)
}

pub fn list(path: &Path) -> Result<Vec<Entry>, String> {
    let dir = open_dir(path)?;
    let mut out = Vec::new();
    for name in names(&dir, path)? {
        // A name removed since the read is skipped, as ls does.
        let Ok(st) = statat(&dir, &name, AtFlags::SYMLINK_NOFOLLOW) else { continue };
        let kind = match kind_of(&st) {
            FileType::Directory => "dir",
            FileType::RegularFile => "file",
            FileType::Symlink => "link",
            _ => "other",
        };
        out.push(Entry {
            name: name.to_string_lossy().into_owned(),
            kind,
            size: st.st_size.cast_unsigned(),
            mtime_ns: st.st_mtime * 1_000_000_000 + st.st_mtime_nsec.cast_signed(),
            mode: st.st_mode & 0o7777,
            uid: st.st_uid,
            gid: st.st_gid,
            lossy: name.to_str().is_none(),
        });
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(out)
}

fn valid_name(name: &str) -> bool {
    !name.is_empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0')
}

/// Copies `name` in `from` to `to_name` in `to`: folders (recursively), regular files and links; it never replaces.
fn copy_at(from: &OwnedFd, name: &OsStr, to: &OwnedFd, to_name: &OsStr, shown: &Path) -> Result<u64, String> {
    let err = err_at(shown);
    let st = statat(from, name, AtFlags::SYMLINK_NOFOLLOW).map_err(&err)?;
    let mode = Mode::from_raw_mode(st.st_mode & 0o7777);
    match kind_of(&st) {
        FileType::Directory => {
            let src = enter(from, name, &st, shown)?;
            mkdirat(to, to_name, Mode::RWXU).map_err(&err)?;
            let copied = (|| {
                let dst = openat(to, to_name, DIR_FLAGS, Mode::empty()).map_err(&err)?;
                let mut count = 1;
                for child in names(&src, shown)? {
                    count += copy_at(&src, &child, &dst, &child, &shown.join(&child))?;
                }
                fchmod(&dst, mode).map_err(&err)?;
                Ok(count)
            })();
            copied.map_err(|e| undo(to, to_name, shown, e))
        }
        FileType::RegularFile => {
            let src =
                openat(from, name, OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC, Mode::empty()).map_err(&err)?;
            let now = fstat(&src).map_err(&err)?;
            if now.st_ino != st.st_ino || now.st_dev != st.st_dev {
                return Err(format!("{}: changed while it was being copied", shown.display()));
            }
            let flags = OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC;
            let dst = openat(to, to_name, flags, Mode::RUSR | Mode::WUSR).map_err(&err)?;
            let (mut src, mut dst) = (File::from(src), File::from(dst));
            let copied = io::copy(&mut src, &mut dst)
                .map_err(|e| format!("{}: {e}", shown.display()))
                .and_then(|_| fchmod(&dst, mode).map_err(&err));
            copied.map(|()| 1).map_err(|e| undo(to, to_name, shown, e))
        }
        FileType::Symlink => {
            let target = readlinkat(from, name, Vec::new()).map_err(&err)?;
            symlinkat(target.as_c_str(), to, to_name).map_err(&err)?;
            Ok(1)
        }
        _ => Err(format!("{}: a device, pipe or socket is not copied", shown.display())),
    }
}

/// Removes what a failed `copy_at` made at `to_name`, so the copy can run again.
fn undo(to: &OwnedFd, to_name: &OsStr, shown: &Path, failure: String) -> String {
    match delete_at(to, to_name, shown) {
        Ok(_) => failure,
        Err(e) => format!("{failure}; the partial copy stays: {e}"),
    }
}

fn delete_at(dir: &OwnedFd, name: &OsStr, shown: &Path) -> Result<u64, String> {
    let st = statat(dir, name, AtFlags::SYMLINK_NOFOLLOW).map_err(err_at(shown))?;
    if kind_of(&st) != FileType::Directory {
        unlinkat(dir, name, AtFlags::empty()).map_err(err_at(shown))?;
        return Ok(1);
    }
    let child = enter(dir, name, &st, shown)?;
    let mut count = 1;
    for part in names(&child, shown)? {
        count += delete_at(&child, &part, &shown.join(&part))?;
    }
    unlinkat(dir, name, AtFlags::REMOVEDIR).map_err(err_at(shown))?;
    Ok(count)
}

fn checked_all(paths: &[String]) -> Result<Vec<PathBuf>, String> {
    if paths.is_empty() {
        return Err("no paths".to_owned());
    }
    paths.iter().map(|p| checked(p)).collect()
}

fn transfer(sources: &[String], dest: &str, keep_source: bool) -> Result<u64, String> {
    let sources = checked_all(sources)?;
    let dest = if dest == "/" { PathBuf::from("/") } else { checked(dest)? };
    let to = open_dir(&dest)?;
    let mut count = 0;
    for source in &sources {
        let (from, name) = open_parent(source)?;
        if !keep_source {
            match renameat_with(&from, &name, &to, &name, RenameFlags::NOREPLACE) {
                Ok(()) => {
                    count += 1;
                    continue;
                }
                Err(rustix::io::Errno::XDEV) => {}
                Err(e) => return Err(format!("{}: {e}", source.display())),
            }
        }
        count += copy_at(&from, &name, &to, &name, source)?;
        if !keep_source {
            delete_at(&from, &name, source)?;
        }
    }
    Ok(count)
}

fn run(op: Op) -> Result<Value, String> {
    let count = |n: u64| json!({ "count": n });
    match op {
        Op::List { path } => {
            let path = if path == "/" { PathBuf::from("/") } else { checked(&path)? };
            Ok(json!({ "entries": list(&path)? }))
        }
        Op::Mkdir { path } => {
            let path = checked(&path)?;
            let (dir, name) = open_parent(&path)?;
            mkdirat(&dir, &name, Mode::from_raw_mode(0o755)).map_err(err_at(&path))?;
            Ok(count(1))
        }
        Op::Rename { path, name } => {
            if !valid_name(&name) {
                return Err(format!("{name}: not a valid name"));
            }
            let path = checked(&path)?;
            let (dir, old) = open_parent(&path)?;
            renameat_with(&dir, &old, &dir, name.as_str(), RenameFlags::NOREPLACE).map_err(err_at(&path))?;
            Ok(count(1))
        }
        Op::Copy { sources, dest } => transfer(&sources, &dest, true).map(count),
        Op::Move { sources, dest } => transfer(&sources, &dest, false).map(count),
        Op::Delete { paths } => {
            let mut n = 0;
            for path in checked_all(&paths)? {
                let (dir, name) = open_parent(&path)?;
                n += delete_at(&dir, &name, &path)?;
            }
            Ok(count(n))
        }
        Op::Chmod { paths, mode, mask, recursive } => {
            let change = Change::Mode { mode: mode & 0o7777, mask: mask & 0o7777 };
            let mut n = 0;
            for path in checked_all(&paths)? {
                apply(&path, &change, recursive, &mut n)?;
            }
            Ok(count(n))
        }
        Op::Chown { paths, uid, gid, recursive } => {
            if uid.is_none() && gid.is_none() || uid == Some(u32::MAX) || gid == Some(u32::MAX) {
                return Err("chown needs an owner or a group, and not -1".to_owned());
            }
            let change = Change::Owner { uid, gid };
            let mut n = 0;
            for path in checked_all(&paths)? {
                apply(&path, &change, recursive, &mut n)?;
            }
            Ok(count(n))
        }
    }
}

/// One reply line for one request line; a line that is not a request gets id 0.
pub fn reply(line: &str) -> String {
    let (id, result) = match serde_json::from_str::<Request>(line) {
        Ok(request) => (request.id, run(request.op)),
        Err(e) => (0, Err(format!("bad request: {e}"))),
    };
    let mut out = match result {
        Ok(Value::Object(fields)) => Value::Object(fields),
        Ok(_) => json!({}),
        Err(error) => json!({ "ok": false, "error": error }),
    };
    out["id"] = json!(id);
    if out.get("ok").is_none() {
        out["ok"] = json!(true);
    }
    out.to_string()
}

pub fn serve(input: impl BufRead, mut output: impl Write) -> io::Result<()> {
    writeln!(output, "{}", json!({ "ready": true }))?;
    output.flush()?;
    for line in input.lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        writeln!(output, "{}", reply(&line))?;
        output.flush()?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::{MetadataExt, PermissionsExt};

    fn ask(line: &str) -> Value {
        serde_json::from_str(&reply(line)).expect("json")
    }

    #[test]
    fn a_session_lists_makes_copies_moves_renames_and_deletes() {
        let root = tempfile::tempdir().expect("tmp");
        let r = root.path().display().to_string();
        fs::create_dir(root.path().join("src")).expect("mkdir");
        fs::write(root.path().join("src/a.txt"), "abc").expect("write");
        fs::set_permissions(root.path().join("src/a.txt"), fs::Permissions::from_mode(0o640)).expect("chmod");
        std::os::unix::fs::symlink("/etc/hostname", root.path().join("src/link")).expect("link");

        let out = ask(&format!(r#"{{"id":1,"op":"mkdir","path":"{r}/dst"}}"#));
        assert_eq!((out["id"].as_u64(), out["ok"].as_bool()), (Some(1), Some(true)), "{out}");
        let out = ask(&format!(r#"{{"id":2,"op":"copy","sources":["{r}/src"],"dest":"{r}/dst"}}"#));
        assert_eq!(out["count"], 3, "{out}");
        assert_eq!(fs::read_to_string(root.path().join("dst/src/a.txt")).expect("copied"), "abc");
        assert_eq!(fs::metadata(root.path().join("dst/src/a.txt")).expect("meta").mode() & 0o777, 0o640);
        assert_eq!(fs::read_link(root.path().join("dst/src/link")).expect("link"), Path::new("/etc/hostname"));

        let out = ask(&format!(r#"{{"id":3,"op":"copy","sources":["{r}/src"],"dest":"{r}/dst"}}"#));
        assert_eq!(out["ok"], false, "a copy never replaces: {out}");

        let out = ask(&format!(r#"{{"id":4,"op":"rename","path":"{r}/dst/src","name":"b"}}"#));
        assert_eq!(out["ok"], true, "{out}");
        let out = ask(&format!(r#"{{"id":5,"op":"rename","path":"{r}/dst/b","name":"../x"}}"#));
        assert_eq!(out["ok"], false);

        let out = ask(&format!(r#"{{"id":6,"op":"move","sources":["{r}/src/a.txt"],"dest":"{r}/dst/b"}}"#));
        assert_eq!(out["ok"], false, "a move never replaces: {out}");
        let out = ask(&format!(r#"{{"id":7,"op":"move","sources":["{r}/src/a.txt"],"dest":"{r}"}}"#));
        assert_eq!(out["ok"], true, "{out}");
        assert!(root.path().join("a.txt").exists() && !root.path().join("src/a.txt").exists());

        let out = ask(&format!(r#"{{"id":8,"op":"list","path":"{r}/dst/b"}}"#));
        let names: Vec<_> = out["entries"].as_array().expect("entries").iter().map(|e| e["name"].clone()).collect();
        assert_eq!(names, [json!("a.txt"), json!("link")]);
        assert_eq!(out["entries"][1]["kind"], "link");

        let out = ask(&format!(r#"{{"id":9,"op":"delete","paths":["{r}/dst"]}}"#));
        assert_eq!(out["count"], 4, "{out}");
        assert!(!root.path().join("dst").exists());
        assert!(Path::new("/etc/hostname").exists(), "a link target is never deleted");
    }

    #[test]
    fn a_link_on_the_path_is_not_followed() {
        let root = tempfile::tempdir().expect("tmp");
        fs::create_dir(root.path().join("real")).expect("mkdir");
        fs::write(root.path().join("real/f"), "x").expect("write");
        std::os::unix::fs::symlink(root.path().join("real"), root.path().join("via")).expect("link");
        let r = root.path().display();
        let out = ask(&format!(r#"{{"id":1,"op":"delete","paths":["{r}/via/f"]}}"#));
        assert_eq!(out["ok"], false, "{out}");
        assert!(root.path().join("real/f").exists());
        let out = ask(&format!(r#"{{"id":2,"op":"list","path":"{r}/via"}}"#));
        assert_eq!(out["ok"], false, "{out}");
    }

    #[test]
    fn a_failed_copy_leaves_nothing_so_a_retry_can_succeed() {
        let root = tempfile::tempdir().expect("tmp");
        let to = open_dir(root.path()).expect("dest");
        let proc_dir = open_dir(&Path::new("/proc").join(std::process::id().to_string())).expect("proc");
        let shown = Path::new("/proc/self/mem");
        assert!(copy_at(&proc_dir, OsStr::new("mem"), &to, OsStr::new("f"), shown).is_err(), "mem at 0 reads EIO");
        assert!(!root.path().join("f").exists(), "a failed file copy is removed");

        fs::create_dir(root.path().join("d")).expect("mkdir");
        fs::write(root.path().join("d/a"), "a").expect("write");
        fs::write(root.path().join("d/b"), "b").expect("write");
        fs::set_permissions(root.path().join("d/b"), fs::Permissions::from_mode(0)).expect("chmod");
        if fs::File::open(root.path().join("d/b")).is_ok() {
            return;
        }
        let from = open_dir(root.path()).expect("from");
        assert!(copy_at(&from, OsStr::new("d"), &to, OsStr::new("e"), Path::new("d")).is_err(), "b is unreadable");
        assert!(!root.path().join("e").exists(), "a failed folder copy is removed with what it copied");
    }

    #[test]
    fn bad_lines_get_an_error_reply_and_serve_answers_in_order() {
        assert_eq!(ask("nonsense")["ok"], false);
        assert_eq!(ask(r#"{"id":4,"op":"list","path":"relative"}"#)["id"], 4);
        let mut out = Vec::new();
        serve(io::Cursor::new("{\"id\":1,\"op\":\"list\",\"path\":\"/\"}\n\n{\"id\":2,\"op\":\"nope\"}\n"), &mut out)
            .expect("serve");
        let lines: Vec<Value> =
            String::from_utf8(out).expect("utf8").lines().map(|l| serde_json::from_str(l).expect("json")).collect();
        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0]["ready"], true);
        assert_eq!((lines[1]["id"].as_u64(), lines[1]["ok"].as_bool()), (Some(1), Some(true)));
        assert_eq!((lines[2]["id"].as_u64(), lines[2]["ok"].as_bool()), (Some(0), Some(false)));
    }
}
