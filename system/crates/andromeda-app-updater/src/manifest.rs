use anyhow::{Context, Result, bail};
use serde::Deserialize;
use std::cmp::Ordering;

/// The signed description of the newest build: `<App>-macos-arm64.json` beside the zip.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub version: String,
    pub url: String,
    /// The zip's SHA-256, lower-case hex.
    pub sha256: String,
}

impl Manifest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        let manifest: Self = serde_json::from_slice(bytes).context("manifest is not valid JSON")?;
        parse_version(&manifest.version)?;
        if !manifest.url.starts_with("https://") {
            bail!("manifest url is not https: {}", manifest.url);
        }
        if manifest.sha256.len() != 64 || !manifest.sha256.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
            bail!("manifest sha256 is not 64 lower-case hex digits");
        }
        Ok(manifest)
    }
}

/// A dotted numeric version: a calendar version (`2026.11.18`) and semver (`0.2.20`) both parse. Missing parts count as 0.
pub fn parse_version(text: &str) -> Result<Vec<u64>> {
    if text.is_empty() {
        bail!("empty version");
    }
    text.split('.')
        .map(|part| {
            if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
                bail!("version part {part:?} of {text:?} is not a number");
            }
            part.parse::<u64>().with_context(|| format!("version part {part:?} is too large"))
        })
        .collect()
}

pub fn compare_versions(a: &str, b: &str) -> Result<Ordering> {
    let (a, b) = (parse_version(a)?, parse_version(b)?);
    let len = a.len().max(b.len());
    let at = |v: &[u64], i: usize| v.get(i).copied().unwrap_or(0);
    Ok((0..len).map(|i| at(&a, i).cmp(&at(&b, i))).find(|o| o.is_ne()).unwrap_or(Ordering::Equal))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compares_numerically_not_as_text() {
        assert_eq!(compare_versions("2026.11.18", "2026.11.9").unwrap(), Ordering::Greater);
        assert_eq!(compare_versions("0.2.20", "0.2.3").unwrap(), Ordering::Greater);
        assert_eq!(compare_versions("2026.11", "2026.11.0").unwrap(), Ordering::Equal);
        assert_eq!(compare_versions("2026.11.18", "2026.12.1").unwrap(), Ordering::Less);
    }

    #[test]
    fn rejects_versions_that_are_not_dotted_numbers() {
        for bad in ["", "1..2", "2026.11.18.r3", "v1.2", "1.2-rc1", "99999999999999999999"] {
            assert!(parse_version(bad).is_err(), "{bad:?} parsed");
        }
    }

    #[test]
    fn rejects_a_manifest_with_plain_http_or_a_bad_hash() {
        let hash = "a".repeat(64);
        let good = format!(r#"{{"version":"1.2","url":"https://x/y.zip","sha256":"{hash}"}}"#);
        assert!(Manifest::parse(good.as_bytes()).is_ok());
        let http = good.replace("https://", "http://");
        assert!(Manifest::parse(http.as_bytes()).is_err());
        let upper = good.replace(&hash, &"A".repeat(64));
        assert!(Manifest::parse(upper.as_bytes()).is_err());
        let extra = good.replace('}', r#","channel":"beta"}"#);
        assert!(Manifest::parse(extra.as_bytes()).is_err());
    }
}
