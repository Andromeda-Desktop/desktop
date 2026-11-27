//! Replaces an app bundle with a staged one so that, after a crash or power loss at any point, the bundle on disk is
//! one whole release: the old one or the new one. Every step is one rename or an idempotent removal on one volume;
//! `recover` reads which step the disk reached from what exists and finishes or rolls back.

use anyhow::{Context, Result, bail};
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};

/// Where each bundle lives. `staged` and `ready` sit in the update cache, on the app's volume.
#[derive(Debug, Clone)]
pub struct Paths {
    pub app: PathBuf,
    /// The running release while the new one moves in: `<app>.old`.
    pub old: PathBuf,
    /// The unpacked, verified new bundle.
    pub staged: PathBuf,
    /// Written after `staged` passed every check; holds its version. Without it `staged` is not trusted.
    pub ready: PathBuf,
}

impl Paths {
    pub fn new(app: &Path, stage_dir: &Path) -> Result<Self> {
        let name = app.file_name().context("app path has no file name")?;
        let mut old = app.as_os_str().to_owned();
        old.push(".old");
        Ok(Self { app: app.to_owned(), old: old.into(), staged: stage_dir.join(name), ready: stage_dir.join("ready") })
    }
}

/// What the disk shows, derived only from which paths exist.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Disk {
    /// The release runs; nothing usable is staged.
    Idle,
    /// A verified release waits in the cache.
    Ready,
    /// The running release moved aside; the new one has not moved in.
    MidSwap { ready: bool },
    /// The new release is in place; the old one still waits for removal.
    Swapped,
}

pub fn disk(p: &Paths) -> Result<Disk> {
    let ready = exists(&p.ready)? && exists(&p.staged)?;
    Ok(match (exists(&p.app)?, exists(&p.old)?) {
        (true, false) if ready => Disk::Ready,
        (true, false) => Disk::Idle,
        (false, true) => Disk::MidSwap { ready },
        (true, true) => Disk::Swapped,
        (false, false) => bail!("neither {} nor {} exists", p.app.display(), p.old.display()),
    })
}

/// Marks `staged` as verified. Atomic: the marker appears whole or not at all.
pub fn mark_ready(p: &Paths, version: &str) -> Result<()> {
    let tmp = p.ready.with_extension("tmp");
    fs::write(&tmp, version).with_context(|| format!("cannot write {}", tmp.display()))?;
    fs::rename(&tmp, &p.ready).with_context(|| format!("cannot write {}", p.ready.display()))
}

/// The swap steps, in order. A crash between any two leaves a disk that `recover` repairs.
pub const STEPS: usize = 4;

pub fn step(p: &Paths, index: usize) -> Result<()> {
    match index {
        0 => rename(&p.app, &p.old),
        1 => rename(&p.staged, &p.app),
        2 => remove_file(&p.ready),
        3 => remove_dir(&p.old),
        _ => bail!("no swap step {index}"),
    }
}

/// Moves the ready release into place. The app must not run.
pub fn swap(p: &Paths) -> Result<()> {
    if disk(p)? != Disk::Ready {
        bail!("nothing verified is staged");
    }
    (0..STEPS).try_for_each(|i| step(p, i))
}

/// Brings the disk back to Idle or Ready from any state a crash leaves, finishing a swap whose new release is
/// verified and rolling back one whose is not. Safe to run again after a crash inside it.
pub fn recover(p: &Paths) -> Result<Disk> {
    match disk(p)? {
        Disk::Ready => {}
        Disk::Idle => {
            remove_file(&p.ready)?;
            remove_dir(&p.staged)?;
        }
        Disk::MidSwap { ready: true } => (1..STEPS).try_for_each(|i| step(p, i))?,
        Disk::MidSwap { ready: false } => {
            rename(&p.old, &p.app)?;
            remove_file(&p.ready)?;
            remove_dir(&p.staged)?;
        }
        Disk::Swapped => (2..STEPS).try_for_each(|i| step(p, i))?,
    }
    disk(p)
}

fn exists(path: &Path) -> Result<bool> {
    path.try_exists().with_context(|| format!("cannot read {}", path.display()))
}

fn rename(from: &Path, to: &Path) -> Result<()> {
    fs::rename(from, to).with_context(|| format!("cannot move {} to {}", from.display(), to.display()))
}

fn remove_file(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != ErrorKind::NotFound => Err(e).with_context(|| format!("cannot remove {}", path.display())),
        _ => Ok(()),
    }
}

fn remove_dir(path: &Path) -> Result<()> {
    match fs::remove_dir_all(path) {
        Err(e) if e.kind() != ErrorKind::NotFound => Err(e).with_context(|| format!("cannot remove {}", path.display())),
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
        let cache = root.path().join("cache");
        fs::create_dir_all(&apps).unwrap();
        fs::create_dir_all(&cache).unwrap();
        let p = Paths::new(&apps.join("Andromeda Terminal.app"), &cache).unwrap();
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
    /// without a swap; then recovery, itself cut after each of its own steps and run again.
    #[test]
    fn every_crash_point_leaves_one_whole_release_and_recovery_settles() {
        let mut cases = 0;
        for staged in [Staged::None, Staged::Partial, Staged::Verified] {
            let swap_points = if matches!(staged, Staged::Verified) { 0..=STEPS } else { 0..=0 };
            for crash_after in swap_points {
                for recover_runs in 1..=2 {
                    let f = fixture(staged);
                    (0..crash_after).for_each(|i| step(&f.p, i).unwrap());
                    let swapped_in = crash_after >= 2;
                    let mut end = Disk::Idle;
                    for _ in 0..recover_runs {
                        end = recover(&f.p).unwrap();
                    }
                    cases += 1;
                    let what = format!("{staged:?}, crash after {crash_after} steps, {recover_runs} recover runs");
                    let shown = release(&f.p.app);
                    assert!(shown.as_deref() == Some(OLD) || shown.as_deref() == Some(NEW), "{what}: app is {shown:?}");
                    assert!(!f.p.old.exists(), "{what}: the old bundle stayed");
                    match staged {
                        // A verified release that began to move in always ends in place.
                        Staged::Verified if crash_after >= 1 || swapped_in => {
                            assert_eq!(shown.as_deref(), Some(NEW), "{what}");
                            assert_eq!(end, Disk::Idle, "{what}");
                        }
                        Staged::Verified => assert_eq!(end, Disk::Ready, "{what}"),
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

    /// Power loss after the old bundle moved aside but before the marker existed cannot happen through `swap`
    /// (it needs Ready), but a marker lost to a disk error must roll back, never move an unverified bundle in.
    #[test]
    fn a_mid_swap_without_the_marker_rolls_back() {
        let f = fixture(Staged::Verified);
        step(&f.p, 0).unwrap();
        fs::remove_file(&f.p.ready).unwrap();
        assert_eq!(disk(&f.p).unwrap(), Disk::MidSwap { ready: false });
        assert_eq!(recover(&f.p).unwrap(), Disk::Idle);
        assert_eq!(release(&f.p.app).as_deref(), Some(OLD));
    }

    #[test]
    fn swap_refuses_an_unverified_stage() {
        let f = fixture(Staged::Partial);
        assert!(swap(&f.p).is_err());
        assert_eq!(release(&f.p.app).as_deref(), Some(OLD));
    }

    #[test]
    fn a_full_swap_installs_the_new_release_and_cleans_up() {
        let f = fixture(Staged::Verified);
        swap(&f.p).unwrap();
        assert_eq!(release(&f.p.app).as_deref(), Some(NEW));
        assert_eq!(disk(&f.p).unwrap(), Disk::Idle);
        assert!(!f.p.ready.exists() && !f.p.staged.exists() && !f.p.old.exists());
    }
}
