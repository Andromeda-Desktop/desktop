//! Trust checks: the manifest's `OpenPGP` signature by the release key, and the download's SHA-256.

use crate::manifest::Manifest;
use anyhow::{Context, Result, bail};
use pgp::composed::{Deserializable, DetachedSignature, SignedPublicKey};
use sha2::{Digest, Sha256};
use std::fmt::Write;
use std::io::Read;

/// The public half of the release key (ed25519, 97DD7B3B28BE58AC8787E32D11437B48C42663B5).
pub const RELEASE_KEY: &str = include_str!("../../../../andromeda-release.asc");

/// Parses the manifest only after `signature` (a binary detached signature, as `gpg --detach-sign` writes) by
/// `key` (armored) verifies over its exact bytes.
pub fn signed_manifest(manifest: &[u8], signature: &[u8], key: &str) -> Result<Manifest> {
    let (key, _) = SignedPublicKey::from_string(key).context("cannot read the release key")?;
    let signature = DetachedSignature::from_bytes(signature).context("cannot read the manifest signature")?;
    signature.verify(&key, manifest).map_err(|e| anyhow::anyhow!("manifest signature does not verify: {e}"))?;
    Manifest::parse(manifest)
}

/// Lower-case hex SHA-256 of everything `reader` yields.
pub fn sha256_hex(mut reader: impl Read) -> Result<String> {
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = reader.read(&mut buf).context("cannot read the download")?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hasher.finalize().iter().fold(String::with_capacity(64), |mut hex, b| {
        let _ = write!(hex, "{b:02x}");
        hex
    }))
}

pub fn check_sha256(reader: impl Read, expected: &str) -> Result<()> {
    let actual = sha256_hex(reader)?;
    if actual != expected {
        bail!("download sha256 {actual} does not match the manifest's {expected}");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: &str = include_str!("../tests/fixtures/test-key.asc");
    const MANIFEST: &[u8] = include_bytes!("../tests/fixtures/manifest.json");
    const SIG: &[u8] = include_bytes!("../tests/fixtures/manifest.json.sig");

    #[test]
    fn a_gpg_signature_by_the_key_verifies() {
        assert_eq!(signed_manifest(MANIFEST, SIG, KEY).unwrap().version, "2026.11.19");
    }

    #[test]
    fn a_changed_manifest_or_another_key_is_refused() {
        let tampered = String::from_utf8(MANIFEST.to_vec()).unwrap().replace("2026.11.19", "2026.11.20");
        assert!(signed_manifest(tampered.as_bytes(), SIG, KEY).is_err());
        assert!(signed_manifest(MANIFEST, SIG, RELEASE_KEY).is_err());
        assert!(signed_manifest(MANIFEST, b"", KEY).is_err());
    }
}
