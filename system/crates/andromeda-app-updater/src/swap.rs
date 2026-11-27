//! Replaces an app bundle with a staged one so that, after a crash or power loss at any point, the app path holds
//! one whole release: the old one or the new one. The new release moves in with one atomic exchange of two
//! directory entries; `recover` reads which step the disk reached and finishes or cleans up.

use anyhow::{Context, Result, bail};
use rustix::fs::{CWD, RenameFlags, renameat_with};
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};

/// Where each bundle lives. The stage directory must sit on the app's volume: the exchange cannot cross volumes.
#[derive(Debug, Clone)]
pub struct Paths {
    pub app: PathBuf,
    /// The unpacked, verified new bundle; after the exchange, the old one.
    pub staged: PathBuf,
    /// Written after `staged` passed every check; holds its version. Without it `staged` is not trusted.
    pub ready: PathBuf,
}

impl Paths {
    pub fn new(app: &Path, stage_dir: &Path) -> Result<Self> {
        let name = app.file_name().context("app path has no file name")?;
        Ok(Self { app: app.to_owned(), staged: stage_dir.join(name), ready: stage_dir.join("ready") })
    }
}

/// What the disk shows, derived from which paths exist and which release the app path holds.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Disk {
    /// The release runs; nothing usable is staged.
    Idle,
    /// A verified release waits in the stage directory.
    Ready(String),
    /// The new release is in place; the old one waits in the stage directory for removal.
    Swapped,
}

/// `version_of` reads the release a bundle holds (its `CFBundleShortVersionString`).
pub fn disk(p: &Paths, version_of: impl Fn(&Path) -> Result<String>) -> Result<Disk> {
    if !exists(&p.ready)? || !exists(&p.staged)? {
        return Ok(Disk::Idle);
    }
    let marked = fs::read_to_string(&p.ready).with_context(|| format!("cannot read {}", p.ready.display()))?;
    Ok(if version_of(&p.app)? == marked { Disk::Swapped } else { Disk::Ready(marked) })
}

/// Marks `staged` as verified. Atomic: the marker appears whole or not at all.
pub fn mark_ready(p: &Paths, version: &str) -> Result<()> {
    let tmp = p.ready.with_extension("tmp");
    fs::write(&tmp, version).with_context(|| format!("cannot write {}", tmp.display()))?;
    fs::rename(&tmp, &p.ready).with_context(|| format!("cannot write {}", p.ready.display()))
}

/// The swap steps, in order. A crash between any two leaves a disk that `recover` repairs.
pub const STEPS: usize = 3;

pub fn step(p: &Paths, index: usize) -> Result<()> {
    match index {
        0 => renameat_with(CWD, &p.staged, CWD, &p.app, RenameFlags::EXCHANGE)
            .with_context(|| format!("cannot exchange {} with {}", p.staged.display(), p.app.display())),
        1 => remove_file(&p.ready),
        2 => remove_dir(&p.staged),
        _ => bail!("no swap step {index}"),
    }
}

/// Moves the ready release into place. The app must not run. A failed exchange changes nothing.
pub fn swap(p: &Paths, version_of: impl Fn(&Path) -> Result<String>) -> Result<()> {
    if !matches!(disk(p, version_of)?, Disk::Ready(_)) {
        bail!("nothing verified is staged");
    }
    (0..STEPS).try_for_each(|i| step(p, i))
}

/// Brings the disk back to Idle or Ready from any state a crash leaves. Safe to run again after a crash inside it.
pub fn recover(p: &Paths, version_of: impl Fn(&Path) -> Result<String> + Copy) -> Result<Disk> {
    match disk(p, version_of)? {
        Disk::Ready(_) => {}
        Disk::Swapped => (1..STEPS).try_for_each(|i| step(p, i))?,
        Disk::Idle => discard(p)?,
    }
    disk(p, version_of)
}

/// Drops a staged release that is no longer newer than the app. Call only from Ready.
pub fn discard(p: &Paths) -> Result<()> {
    remove_file(&p.ready)?;
    remove_dir(&p.staged)
}

fn exists(path: &Path) -> Result<bool> {
    path.try_exists().with_context(|| format!("cannot read {}", path.display()))
}

fn remove_file(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != ErrorKind::NotFound => {
            Err(e).with_context(|| format!("cannot remove {}", path.display()))
        }
        _ => Ok(()),
    }
}

fn remove_dir(path: &Path) -> Result<()> {
    match fs::remove_dir_all(path) {
        Err(e) if e.kind() != ErrorKind::NotFound => {
            Err(e).with_context(|| format!("cannot remove {}", path.display()))
        }
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OLD: &str = "2026.11.18";
    const NEW: &str = "2026.11.19";

    struct Fixture {
        _root: tempfile::TempDir,
        p: Paths,
    }

    /// A bundle whose release is written in two files, so a half-moved bundle would show two versions.
    fn bundle(dir: &Path, version: &str) {
        fs::create_dir_all(dir.join("Contents/MacOS")).unwrap();
        fs::write(dir.join("Contents/version"), version).unwrap();
        fs::write(dir.join("Contents/MacOS/app"), version).unwrap();
    }

    fn release(dir: &Path) -> Option<String> {
        let a = fs::read_to_string(dir.join("Contents/version")).ok()?;
        let b = fs::read_to_string(dir.join("Contents/MacOS/app")).ok()?;
        (a == b).then_some(a)
    }

    fn version_of(dir: &Path) -> Result<String> {
        Ok(fs::read_to_string(dir.join("Contents/version"))?)
    }

    /// `staged`: none, half unpacked (no marker), or verified.
    #[derive(Clone, Copy, Debug)]
    enum Staged {
        None,
        Partial,
        Verified,
    }

    fn fixture(staged: Staged) -> Fixture {
        let root = tempfile::tempdir().unwrap();
        let apps = root.path().join("Applications");
        let stage = apps.join(".Andromeda Terminal.app.update");
        fs::create_dir_all(&stage).unwrap();
        let p = Paths::new(&apps.join("Andromeda Terminal.app"), &stage).unwrap();
        bundle(&p.app, OLD);
        match staged {
            Staged::None => {}
            Staged::Partial => fs::create_dir_all(p.staged.join("Contents")).unwrap(),
            Staged::Verified => {
                bundle(&p.staged, NEW);
                mark_ready(&p, NEW).unwrap();
            }
        }
        Fixture { _root: root, p }
    }

    /// Every reachable disk: a crash after k swap steps (the swap only starts from Ready), or any staging state
    /// without a swap; then recovery, itself cut short by running it once or twice. At every crash point the app
    /// path holds one whole release, so the app can start and run `recover`.
    #[test]
    fn every_crash_point_leaves_one_whole_release_and_recovery_settles() {
        let mut cases = 0;
        for staged in [Staged::None, Staged::Partial, Staged::Verified] {
            let swap_points = if matches!(staged, Staged::Verified) { 0..=STEPS } else { 0..=0 };
            for crash_after in swap_points {
                for recover_runs in 1..=2 {
                    let f = fixture(staged);
                    (0..crash_after).for_each(|i| step(&f.p, i).unwrap());
                    let what = format!("{staged:?}, crash after {crash_after} steps, {recover_runs} recover runs");
                    let at_crash = release(&f.p.app);
                    assert!(
                        at_crash.as_deref() == Some(OLD) || at_crash.as_deref() == Some(NEW),
                        "{what}: app is {at_crash:?}"
                    );
                    let mut end = Disk::Idle;
                    for _ in 0..recover_runs {
                        end = recover(&f.p, version_of).unwrap();
                    }
                    cases += 1;
                    let shown = release(&f.p.app);
                    match staged {
                        Staged::Verified if crash_after >= 1 => {
                            assert_eq!(shown.as_deref(), Some(NEW), "{what}");
                            assert_eq!(end, Disk::Idle, "{what}");
                            assert!(!f.p.staged.exists() && !f.p.ready.exists(), "{what}: the old bundle stayed");
                        }
                        Staged::Verified => {
                            assert_eq!(shown.as_deref(), Some(OLD), "{what}");
                            assert_eq!(end, Disk::Ready(NEW.to_owned()), "{what}");
                        }
                        Staged::None | Staged::Partial => {
                            assert_eq!(shown.as_deref(), Some(OLD), "{what}");
                            assert_eq!(end, Disk::Idle, "{what}");
                            assert!(!f.p.staged.exists(), "{what}: the partial unpack stayed");
                        }
                    }
                }
            }
        }
        assert_eq!(cases, 2 * (STEPS + 1) + 2 * 2);
    }

    #[test]
    fn swap_refuses_an_unverified_stage() {
        let f = fixture(Staged::Partial);
        assert!(swap(&f.p, version_of).is_err());
        assert_eq!(release(&f.p.app).as_deref(), Some(OLD));
    }

    #[test]
    fn a_full_swap_installs_the_new_release_and_cleans_up() {
        let f = fixture(Staged::Verified);
        swap(&f.p, version_of).unwrap();
        assert_eq!(release(&f.p.app).as_deref(), Some(NEW));
        assert_eq!(disk(&f.p, version_of).unwrap(), Disk::Idle);
        assert!(!f.p.ready.exists() && !f.p.staged.exists());
    }
}
