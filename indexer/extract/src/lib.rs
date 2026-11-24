//! Text of untrusted documents for the file index: HTML, PDF, office and EPUB archives, photo and audio metadata.
//! Plain text stays in the index daemon (indexer/src/text_extract.cpp).

use lofty::config::ParseOptions;
use lofty::prelude::{ItemKey, TaggedFileExt};
use lofty::probe::Probe;
use nom_exif::{EntryValue, ExifTag, IfdIndex};
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

/// Bytes of text kept per file; indexer/src/extract.h kMaxText.
pub const MAX_TEXT: usize = 256 * 1024;
/// Markup read from one file or archive entry; a larger entry (or a zip bomb) is cut here.
const MAX_MARKUP: u64 = 4 * 1024 * 1024;
/// A PDF is read whole; the index sends files of at most 50 MiB (IndexDb::kMaxContentFile).
const MAX_PDF: u64 = 64 * 1024 * 1024;
/// XMP and IPTC blocks sit near the start of a photo file (JPEG APP segments, at most 64 KiB each, precede the image).
const MAX_PHOTO_HEAD: u64 = 1024 * 1024;

/// What the daemon asks for; the first word of each request line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Html,
    Pdf,
    Zip,
    Photo,
    Audio,
}

impl Kind {
    pub fn from_word(word: &[u8]) -> Option<Self> {
        Some(match word {
            b"html" => Self::Html,
            b"pdf" => Self::Pdf,
            b"zip" => Self::Zip,
            b"photo" => Self::Photo,
            b"audio" => Self::Audio,
            _ => return None,
        })
    }
}

/// The text of a file, at most [`MAX_TEXT`] bytes, or `None` when it cannot be read. A panic in a parser counts as
/// unreadable.
pub fn extract(kind: Kind, path: &Path) -> Option<Vec<u8>> {
    std::panic::catch_unwind(|| match kind {
        Kind::Html => read_head(path, MAX_MARKUP).map(|markup| strip_tags(&markup, MAX_TEXT)),
        Kind::Pdf => pdf_text(path),
        Kind::Zip => zip_text(path),
        Kind::Photo => photo_text(path),
        Kind::Audio => audio_text(path),
    })
    .ok()
    .flatten()
}

fn read_head(path: &Path, max: u64) -> Option<Vec<u8>> {
    let mut data = Vec::new();
    File::open(path).ok()?.take(max).read_to_end(&mut data).ok()?;
    Some(data)
}

fn find(haystack: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    memchr::memmem::find(haystack.get(from..)?, needle).map(|p| p + from)
}

fn tag_is(markup: &[u8], at: usize, name: &[u8]) -> bool {
    markup.get(at..at + name.len()).is_some_and(|s| s.eq_ignore_ascii_case(name))
}

/// Markup without its tags, at most `max` bytes: each tag becomes a space; script and style bodies are dropped;
/// common entities are decoded.
pub fn strip_tags(markup: &[u8], max: usize) -> Vec<u8> {
    const ENTITIES: [(&[u8], u8); 6] =
        [(b"&amp;", b'&'), (b"&lt;", b'<'), (b"&gt;", b'>'), (b"&quot;", b'"'), (b"&apos;", b'\''), (b"&nbsp;", b' ')];
    let mut out = Vec::with_capacity(markup.len().min(max));
    let mut i = 0;
    while i < markup.len() && out.len() < max {
        let c = markup[i];
        if c == b'<' {
            // Script and style bodies are code, not text.
            for skipped in [b"script".as_slice(), b"style"] {
                if tag_is(markup, i + 1, skipped) {
                    i = find(markup, &[b"</", skipped].concat(), i).unwrap_or(markup.len());
                    break;
                }
            }
            i = find(markup, b">", i).map_or(markup.len(), |end| end + 1);
            if out.last().is_some_and(|&b| b != b' ') {
                out.push(b' ');
            }
            continue;
        }
        if c == b'&'
            && let Some((name, ch)) = ENTITIES.iter().find(|(name, _)| markup[i..].starts_with(name))
        {
            out.push(*ch);
            i += name.len();
            continue;
        }
        out.push(c);
        i += 1;
    }
    out
}

fn pdf_text(path: &Path) -> Option<Vec<u8>> {
    let data = read_head(path, MAX_PDF)?;
    // poppler keeps a pointer to data, which outlives doc (locals drop in reverse order).
    let doc = poppler::Document::from_data(&data, None).ok()?;
    let mut text = Vec::new();
    for i in 0..doc.n_pages() {
        if text.len() >= MAX_TEXT {
            break;
        }
        if let Some(page_text) = doc.page(i).and_then(|page| page.text()) {
            text.extend_from_slice(page_text.as_bytes());
            text.push(b'\n');
        }
    }
    text.truncate(MAX_TEXT);
    Some(text)
}

/// The entries of an office or EPUB archive that hold its text, by file extension.
fn text_entries(path: &Path) -> fn(&str) -> bool {
    let ext = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    match ext.as_str() {
        "docx" => |e| e == "word/document.xml",
        "xlsx" => |e| e == "xl/sharedStrings.xml",
        "pptx" => |e| e.starts_with("ppt/slides/slide") && e.ends_with(".xml"),
        "epub" => |e| e.ends_with(".xhtml") || e.ends_with(".html") || e.ends_with(".htm"),
        _ => |e| e == "content.xml", // OpenDocument
    }
}

fn zip_text(path: &Path) -> Option<Vec<u8>> {
    let mut archive = zip::ZipArchive::new(BufReader::new(File::open(path).ok()?)).ok()?;
    let wanted = text_entries(path);
    let mut text = Vec::new();
    let mut markup = Vec::new();
    for i in 0..archive.len() {
        if text.len() >= MAX_TEXT {
            break;
        }
        if !archive.name_for_index(i).and_then(Result::ok).is_some_and(|name| wanted(&name)) {
            continue;
        }
        let Ok(entry) = archive.by_index(i) else { continue };
        markup.clear();
        // A broken stream keeps what was read before the error.
        let _ = entry.take(MAX_MARKUP).read_to_end(&mut markup);
        text.extend(strip_tags(&markup, MAX_TEXT - text.len()));
        text.push(b'\n');
    }
    text.truncate(MAX_TEXT);
    Some(text)
}

/// Adds one metadata value on its own line, unless it would pass [`MAX_TEXT`].
fn append(text: &mut Vec<u8>, value: &[u8]) {
    if value.is_empty() || text.len() + value.len() + 1 > MAX_TEXT {
        return;
    }
    if !text.is_empty() {
        text.push(b'\n');
    }
    text.extend_from_slice(value);
}

fn trimmed(value: &[u8]) -> &[u8] {
    let keep = |b: &u8| *b != 0 && !b.is_ascii_whitespace();
    let start = value.iter().position(keep).unwrap_or(value.len());
    let end = value.iter().rposition(keep).map_or(start, |p| p + 1);
    &value[start..end]
}

/// EXIF UserComment: an 8-byte character set name, then the text.
fn user_comment(bytes: &[u8]) -> Vec<u8> {
    let Some((charset, body)) = bytes.split_at_checked(8) else { return bytes.to_vec() };
    if !charset.starts_with(b"UNICODE") {
        return body.to_vec();
    }
    let big_endian = body.first() == Some(&0);
    let units = body
        .chunks_exact(2)
        .map(|p| if big_endian { u16::from_be_bytes([p[0], p[1]]) } else { u16::from_le_bytes([p[0], p[1]]) });
    char::decode_utf16(units).map(|c| c.unwrap_or(char::REPLACEMENT_CHARACTER)).collect::<String>().into_bytes()
}

fn exif_value(value: &EntryValue) -> Vec<u8> {
    match value {
        EntryValue::Undefined(bytes) => user_comment(bytes),
        other => other.to_string().into_bytes(),
    }
}

/// XMP properties as words: an element's text (every `rdf:li` of a Bag or Alt), or an attribute value.
fn xmp_values(head: &[u8]) -> Vec<Vec<u8>> {
    const PROPERTIES: [&[u8]; 4] = [b"dc:subject", b"dc:description", b"dc:title", b"photoshop:City"];
    let Some(start) = find(head, b"<x:xmpmeta", 0) else { return Vec::new() };
    let end = find(head, b"</x:xmpmeta>", start).unwrap_or(head.len());
    let xmp = &head[start..end];
    let mut values = Vec::new();
    for property in PROPERTIES {
        let open = [b"<", property].concat();
        let element = memchr::memmem::find_iter(xmp, &open)
            .find(|&i| xmp.get(i + open.len()).is_some_and(|&b| b == b'>' || b.is_ascii_whitespace()));
        let raw = if let Some(at) = element {
            let close = find(xmp, &[b"</", property, b">"].concat(), at).unwrap_or(xmp.len());
            &xmp[at..close]
        } else if let Some(at) = find(xmp, &[property, b"=\""].concat(), 0) {
            let from = at + property.len() + 2;
            &xmp[from..find(xmp, b"\"", from).unwrap_or(xmp.len())]
        } else {
            continue;
        };
        let text = strip_tags(raw, MAX_TEXT);
        let words: Vec<&[u8]> = text.split(u8::is_ascii_whitespace).filter(|w| !w.is_empty()).collect();
        values.push(words.join(&b' '));
    }
    values
}

/// IPTC keywords, caption, city and country from the Photoshop IPTC resource (8BIM 0x0404), in file order.
fn iptc_values(head: &[u8]) -> Vec<Vec<u8>> {
    const DATASETS: [u8; 4] = [25, 120, 90, 101];
    let mut values = Vec::new();
    let Some(at) = find(head, b"8BIM\x04\x04", 0) else { return values };
    let resource = &head[at + 6..];
    let Some(&name_len) = resource.first() else { return values };
    let name_end = (1 + usize::from(name_len) + 1) & !1;
    let Some(size) = resource.get(name_end..name_end + 4) else { return values };
    let size = u32::from_be_bytes([size[0], size[1], size[2], size[3]]) as usize;
    let data = &resource[name_end + 4..];
    let data = &data[..size.min(data.len())];
    let mut pos = 0;
    while let Some(&[0x1c, record, dataset, hi, lo]) = data.get(pos..pos + 5) {
        pos += 5;
        let mut len = usize::from(u16::from_be_bytes([hi, lo]));
        if len & 0x8000 != 0 {
            let count = len & 0x7fff;
            let Some(bytes) = data.get(pos..pos + count).filter(|_| count <= 4) else { break };
            len = bytes.iter().fold(0, |n, &b| (n << 8) | usize::from(b));
            pos += count;
        }
        let Some(value) = data.get(pos..pos.saturating_add(len)) else { break };
        pos += len;
        if record == 2 && DATASETS.contains(&dataset) {
            values.push(value.to_vec());
        }
    }
    values
}

/// Who, what and when; numeric shooting data (exposure, focal length) is left out as search noise.
fn photo_text(path: &Path) -> Option<Vec<u8>> {
    const ARTIST: u16 = 0x013b;
    let mut text = Vec::new();
    if let Ok(exif) = nom_exif::read_exif(path) {
        let values = [
            exif.get(ExifTag::Make),
            exif.get(ExifTag::Model),
            exif.get(ExifTag::LensModel),
            exif.get(ExifTag::DateTimeOriginal),
            exif.get_by_code(IfdIndex::MAIN, ARTIST),
            exif.get(ExifTag::ImageDescription),
            exif.get(ExifTag::UserComment),
        ];
        for value in values.into_iter().flatten() {
            append(&mut text, trimmed(&exif_value(value)));
        }
    }
    let head = read_head(path, MAX_PHOTO_HEAD)?;
    for value in xmp_values(&head).into_iter().chain(iptc_values(&head)) {
        append(&mut text, trimmed(&value));
    }
    (!text.is_empty()).then_some(text)
}

/// Each key's values come from the first tag that has the key, primary tag first (as TagLib's tag union).
fn audio_text(path: &Path) -> Option<Vec<u8>> {
    const KEYS: [ItemKey; 8] = [
        ItemKey::TrackTitle,
        ItemKey::TrackArtist,
        ItemKey::AlbumTitle,
        ItemKey::AlbumArtist,
        ItemKey::Composer,
        ItemKey::Genre,
        ItemKey::RecordingDate,
        ItemKey::Comment,
    ];
    let file = Probe::open(path)
        .ok()?
        .options(ParseOptions::new().read_properties(false))
        .guess_file_type()
        .ok()?
        .read()
        .ok()?;
    let primary = file.primary_tag();
    let tags: Vec<_> = primary
        .into_iter()
        .chain(file.tags().iter().filter(|t| !primary.is_some_and(|p| std::ptr::eq(p, *t))))
        .collect();
    let mut text = Vec::new();
    for key in KEYS {
        if let Some(tag) = tags.iter().find(|t| t.get_strings(key).next().is_some()) {
            for value in tag.get_strings(key) {
                append(&mut text, trimmed(value.as_bytes()));
            }
        }
    }
    (!text.is_empty()).then_some(text)
}
