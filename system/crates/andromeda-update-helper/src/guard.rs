//! Commit guards. A commit starts only with the boot partitions mounted; a journal holds its targets until the
//! helper sees it end, so a commit cut off by a crash or power loss is repaired by the next helper run.

use std::fs::{self, File};
use std::io::{ErrorKind, Write};
use std::path::Path;

use anyhow::{Context, Result, bail};

pub const JOURNAL: &str = "interrupted";

/// Fails when an fstab mount at /boot, /efi or below them is not mounted: package hooks would write the kernel and
/// initramfs into the empty directory under it, and the next boot would load the old ones.
pub fn boot_mounted() -> Result<()> {
    let fstab = match fs::read_to_string("/etc/fstab") {
        Err(e) if e.kind() == ErrorKind::NotFound => String::new(),
        other => other.context("/etc/fstab")?,
    };
    let points = boot_points(&fstab);
    for point in &points {
        // Opens an x-systemd.automount mount.
        let _ = fs::read_dir(point);
    }
    let missing = unmounted(&points, &fs::read_to_string("/proc/self/mountinfo")?);
    if !missing.is_empty() {
        bail!("{} is not mounted: mount it, then update again", missing.join(", "));
    }
    Ok(())
}

fn boot_points(fstab: &str) -> Vec<String> {
    let under = |p: &str, top: &str| p == top || p.strip_prefix(top).is_some_and(|rest| rest.starts_with('/'));
    fstab
        .lines()
        .map(str::split_whitespace)
        .filter_map(|mut f| Some((f.next()?, f.next()?, f.next()?, f.next()?)))
        .filter(|(spec, point, _, opts)| {
            !spec.starts_with('#')
                && (under(point, "/boot") || under(point, "/efi"))
                && !opts.split(',').any(|o| o == "noauto")
        })
        .map(|(_, point, _, _)| point.to_owned())
        .collect()
}

fn unmounted(points: &[String], mountinfo: &str) -> Vec<String> {
    let mounted: Vec<&str> = mountinfo.lines().filter_map(|l| l.split(' ').nth(4)).collect();
    points.iter().filter(|p| !mounted.contains(&p.as_str())).cloned().collect()
}

/// Written right before the commit, durable before it returns.
pub fn begin(state_dir: &Path, targets: &[String]) -> Result<()> {
    let tmp = state_dir.join(format!("{JOURNAL}.new"));
    let mut file = File::create(&tmp)?;
    for target in targets {
        writeln!(file, "{target}")?;
    }
    file.sync_all()?;
    fs::rename(&tmp, state_dir.join(JOURNAL))?;
    File::open(state_dir)?.sync_all()?;
    Ok(())
}

/// The helper saw the commit return (success or error): nothing to repair.
pub fn end(state_dir: &Path) {
    let _ = fs::remove_file(state_dir.join(JOURNAL));
}

/// Repairs a commit the helper did not see end; a failed repair keeps the journal for the next run.
pub fn recover(state_dir: &Path) -> Result<()> {
    let path = state_dir.join(JOURNAL);
    let (targets, since) = match fs::read_to_string(&path) {
        Err(e) if e.kind() == ErrorKind::NotFound => return Ok(()),
        text => (text?.lines().map(str::to_owned).collect::<Vec<_>>(), fs::metadata(&path)?.modified()?),
    };
    log::warn!("repairing an interrupted update of {} packages", targets.len());
    boot_mounted()?;
    crate::transaction::repair(&targets, since).context("repair of the interrupted update")?;
    end(state_dir);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_unmounted_boot_partitions() {
        let fstab = "UUID=A / ext4 defaults 0 1\nUUID=B /boot vfat umask=0077 0 2\nUUID=C /boot/efi vfat defaults 0 2\n\
                     UUID=D /efi vfat noauto 0 2\n#UUID=E /boot2 vfat defaults 0 2\nUUID=F /bootx vfat defaults 0 2\n";
        let points = boot_points(fstab);
        assert_eq!(points, ["/boot", "/boot/efi"]);
        let mountinfo = "22 1 259:7 / / rw - ext4 /dev/nvme0n1p7 rw\n30 22 259:1 / /boot/efi rw - vfat /dev/x rw\n";
        assert_eq!(unmounted(&points, mountinfo), ["/boot"]);
    }
}
