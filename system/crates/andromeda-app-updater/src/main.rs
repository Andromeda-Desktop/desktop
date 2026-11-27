//! andromeda-app-updater: the macOS self-update helper inside Terminal.app and Link.app (Contents/MacOS).
//!
//! `check <manifest-url>` finishes or cleans up an earlier swap, then stages a newer signed release beside the app
//! and prints `ready <version>`, or prints `none`. `swap <pid> [reopen]` waits for the app to exit, moves the ready
//! release in, restarts the bundle's launchd agents (Link) and, with `reopen`, starts the app again. Errors go to
//! stderr with exit status 1.

use andromeda_app_updater::manifest::{Manifest, compare_versions};
use andromeda_app_updater::swap::{self, Disk, Paths};
use andromeda_app_updater::verify::{RELEASE_KEY, check_sha256, signed_manifest};
use anyhow::{Context, Result, bail};
use rustix::fs::{FlockOperation, flock};
use rustix::process::{Pid, test_kill_process};
use std::cmp::Ordering;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

const TIMEOUT: Duration = Duration::from_secs(60);
const MAX_MANIFEST_BYTES: u64 = 64 * 1024;
const MAX_ZIP_BYTES: u64 = 512 * 1024 * 1024;
const EXIT_WAIT: Duration = Duration::from_secs(30);

fn main() {
    if let Err(e) = run() {
        eprintln!("andromeda-app-updater: {e:#}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let app = own_bundle()?;
    let stage_dir = stage_dir(&app)?;
    let paths = Paths::new(&app, &stage_dir)?;
    // One helper at a time per app: a second check must not clear a stage the first one fills.
    let lock = File::create(stage_dir.join("lock")).context("cannot create the stage lock")?;
    flock(&lock, FlockOperation::LockExclusive).context("cannot lock the stage directory")?;
    match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        ["check", url] => {
            let line = check(&paths, &stage_dir, url)?;
            println!("{line}");
            Ok(())
        }
        ["swap", pid, rest @ ..] if rest.is_empty() || rest == ["reopen"] => {
            swap_after_exit(&paths, pid.parse().context("pid is not a number")?, !rest.is_empty())
        }
        _ => bail!("usage: andromeda-app-updater check <manifest-url> | swap <pid> [reopen]"),
    }
}

/// The `.app` that holds this binary at Contents/MacOS/.
fn own_bundle() -> Result<PathBuf> {
    let exe = std::env::current_exe().context("cannot find this binary")?;
    let app = exe.ancestors().nth(3).context("binary is not inside an app bundle")?;
    if app.extension().is_none_or(|e| e != "app") {
        bail!("{} is not an app bundle", app.display());
    }
    Ok(app.to_owned())
}

/// `.<App>.app.update` beside the bundle: the same volume, so the swap can exchange the two in one step.
fn stage_dir(app: &Path) -> Result<PathBuf> {
    let name = app.file_name().context("app path has no file name")?.to_string_lossy();
    let dir = app.parent().context("app has no parent directory")?.join(format!(".{name}.update"));
    fs::create_dir_all(&dir).with_context(|| format!("cannot create {}", dir.display()))?;
    Ok(dir)
}

fn check(paths: &Paths, stage_dir: &Path, url: &str) -> Result<String> {
    let current = plist_value(&paths.app, "CFBundleShortVersionString")?;
    if let Disk::Ready(staged) = swap::recover(paths, version_of)? {
        if compare_versions(&staged, &current)? == Ordering::Greater {
            return Ok(format!("ready {staged}"));
        }
        swap::discard(paths)?;
    }
    let agent: ureq::Agent = ureq::Agent::config_builder().timeout_global(Some(TIMEOUT)).build().into();
    let manifest = fetch_manifest(&agent, url)?;
    if compare_versions(&manifest.version, &current)? != Ordering::Greater {
        return Ok("none".to_owned());
    }
    stage(&agent, paths, stage_dir, &manifest)?;
    Ok(format!("ready {}", manifest.version))
}

fn fetch_manifest(agent: &ureq::Agent, url: &str) -> Result<Manifest> {
    let get = |url: &str| -> Result<Vec<u8>> {
        agent
            .get(url)
            .call()
            .with_context(|| url.to_owned())?
            .into_body()
            .with_config()
            .limit(MAX_MANIFEST_BYTES)
            .read_to_vec()
            .with_context(|| url.to_owned())
    };
    signed_manifest(&get(url)?, &get(&format!("{url}.sig"))?, RELEASE_KEY)
}

/// Downloads, checks and unpacks the release, then marks it ready. Any failure leaves no marker.
fn stage(agent: &ureq::Agent, paths: &Paths, stage_dir: &Path, manifest: &Manifest) -> Result<()> {
    let zip = stage_dir.join("download.zip");
    let unpack = stage_dir.join("unpack");
    let result = (|| {
        let mut body = agent.get(&manifest.url).call().with_context(|| manifest.url.clone())?.into_body();
        let mut reader = body.with_config().limit(MAX_ZIP_BYTES).reader();
        io::copy(&mut reader, &mut File::create(&zip).context("cannot create the download")?)
            .with_context(|| manifest.url.clone())?;
        check_sha256(File::open(&zip)?, &manifest.sha256)?;
        remove_dir(&unpack)?;
        run_tool(Command::new("ditto").arg("-x").arg("-k").arg(&zip).arg(&unpack))?;
        let name = paths.app.file_name().context("app path has no file name")?;
        let new = unpack.join(name);
        let version = plist_value(&new, "CFBundleShortVersionString")?;
        if version != manifest.version {
            bail!("the download holds release {version}, the manifest names {}", manifest.version);
        }
        if plist_value(&new, "CFBundleIdentifier")? != plist_value(&paths.app, "CFBundleIdentifier")? {
            bail!("the download is another app");
        }
        run_tool(Command::new("codesign").args(["--verify", "--deep", "--strict"]).arg(&new))?;
        remove_dir(&paths.staged)?;
        fs::rename(&new, &paths.staged).context("cannot move the new release into the stage")?;
        swap::mark_ready(paths, &manifest.version)
    })();
    let cleanup = remove_dir(&unpack).and(remove_file(&zip));
    result.and(cleanup)
}

fn swap_after_exit(paths: &Paths, pid: i32, reopen: bool) -> Result<()> {
    let pid = Pid::from_raw(pid).context("pid must be positive")?;
    let start = Instant::now();
    while test_kill_process(pid).is_ok() {
        if start.elapsed() > EXIT_WAIT {
            bail!("the app (pid {pid:?}) did not exit");
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let swapped = swap::recover(paths, version_of)
        .and_then(|_| swap::swap(paths, version_of))
        .and_then(|()| reload_agents(&paths.app));
    if !reopen {
        return swapped;
    }
    // The user asked for a restart: the app starts again with whichever release the disk holds.
    let reopened = run_tool(Command::new("open").arg(&paths.app));
    swapped.and(reopened)
}

/// Link's launchd agents run binaries from the bundle; its install-agents.sh restarts them from the new release.
fn reload_agents(app: &Path) -> Result<()> {
    let script = app.join("Contents/Resources/install-agents.sh");
    if !script.try_exists().with_context(|| format!("cannot read {}", script.display()))? {
        return Ok(());
    }
    run_tool(Command::new(&script).arg(app))
}

fn version_of(app: &Path) -> Result<String> {
    plist_value(app, "CFBundleShortVersionString")
}

fn plist_value(app: &Path, key: &str) -> Result<String> {
    let plist = app.join("Contents/Info.plist");
    let out = Command::new("plutil")
        .args(["-extract", key, "raw", "-o", "-"])
        .arg(&plist)
        .output()
        .context("cannot run plutil")?;
    if !out.status.success() {
        bail!("{} has no {key}", plist.display());
    }
    Ok(String::from_utf8(out.stdout).context("plist value is not UTF-8")?.trim_end().to_owned())
}

fn run_tool(cmd: &mut Command) -> Result<()> {
    let out = cmd.output().with_context(|| format!("cannot run {cmd:?}"))?;
    if !out.status.success() {
        bail!("{cmd:?} failed: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(())
}

fn remove_dir(path: &Path) -> Result<()> {
    match fs::remove_dir_all(path) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => {
            Err(e).with_context(|| format!("cannot remove {}", path.display()))
        }
        _ => Ok(()),
    }
}

fn remove_file(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => {
            Err(e).with_context(|| format!("cannot remove {}", path.display()))
        }
        _ => Ok(()),
    }
}
