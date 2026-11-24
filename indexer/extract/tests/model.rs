//! Random and truncated inputs of every kind: the extractor never panics out, never hangs, and never returns more
//! than MAX_TEXT bytes. The intact samples give their words.

use andromeda_index_extract::{Kind, MAX_TEXT, extract};
use std::io::Write;
use std::path::Path;
use std::sync::mpsc;
use std::time::Duration;

fn zip(entries: &[(&str, Vec<u8>)]) -> Vec<u8> {
    let mut out = std::io::Cursor::new(Vec::new());
    let mut writer = zip::ZipWriter::new(&mut out);
    for (name, data) in entries {
        writer.start_file(*name, zip::write::SimpleFileOptions::default()).unwrap();
        writer.write_all(data).unwrap();
    }
    writer.finish().unwrap();
    out.into_inner()
}

fn pdf(text: &str) -> Vec<u8> {
    let stream = format!("BT /F1 24 Tf 72 720 Td ({text}) Tj ET");
    let objects = [
        "<< /Type /Catalog /Pages 2 0 R >>".to_string(),
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>".to_string(),
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>"
            .to_string(),
        format!("<< /Length {} >>\nstream\n{stream}\nendstream", stream.len()),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>".to_string(),
    ];
    let mut out = b"%PDF-1.4\n".to_vec();
    let mut offsets = Vec::new();
    for (i, body) in objects.iter().enumerate() {
        offsets.push(out.len());
        out.extend(format!("{} 0 obj\n{body}\nendobj\n", i + 1).bytes());
    }
    let xref = out.len();
    out.extend(format!("xref\n0 {}\n0000000000 65535 f \n", objects.len() + 1).bytes());
    for offset in offsets {
        out.extend(format!("{offset:010} 00000 n \n").bytes());
    }
    out.extend(format!("trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n", objects.len() + 1).bytes());
    out
}

fn segment(marker: u8, body: &[u8]) -> Vec<u8> {
    let len = u16::try_from(body.len() + 2).unwrap();
    [&[0xff, marker][..], &len.to_be_bytes(), body].concat()
}

/// A JPEG with an EXIF Model, an XMP subject and an IPTC keyword; no image data (no parser here needs it).
fn jpeg(model: &str, subject: &str, keyword: &str) -> Vec<u8> {
    let value = [model.as_bytes(), b"\0"].concat();
    let count = u32::try_from(value.len()).unwrap();
    let mut tiff = b"II*\0\x08\0\0\0\x01\0".to_vec();
    tiff.extend(0x0110u16.to_le_bytes());
    tiff.extend(2u16.to_le_bytes());
    tiff.extend(count.to_le_bytes());
    tiff.extend(26u32.to_le_bytes());
    tiff.extend(0u32.to_le_bytes());
    tiff.extend(&value);
    let xmp = format!(
        "http://ns.adobe.com/xap/1.0/\0<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description>\
         <dc:subject><rdf:Bag><rdf:li>{subject}</rdf:li></rdf:Bag></dc:subject></rdf:Description></rdf:RDF></x:xmpmeta>"
    );
    let iim = [&[0x1c, 2, 25][..], &u16::try_from(keyword.len()).unwrap().to_be_bytes(), keyword.as_bytes()].concat();
    let iptc =
        [&b"Photoshop 3.0\08BIM\x04\x04\0\0"[..], &u32::try_from(iim.len()).unwrap().to_be_bytes(), &iim].concat();
    [
        &[0xff, 0xd8][..],
        &segment(0xe1, &[b"Exif\0\0", &tiff[..]].concat()),
        &segment(0xe1, xmp.as_bytes()),
        &segment(0xed, &iptc),
        &[0xff, 0xd9],
    ]
    .concat()
}

fn chunk(id: &[u8], body: &[u8]) -> Vec<u8> {
    let mut out = [id, &u32::try_from(body.len()).unwrap().to_le_bytes(), body].concat();
    if body.len() % 2 == 1 {
        out.push(0);
    }
    out
}

/// A mono 8 kHz WAV with a RIFF INFO title.
fn wav(title: &str) -> Vec<u8> {
    let fmt = [
        &1u16.to_le_bytes()[..],
        &1u16.to_le_bytes(),
        &8000u32.to_le_bytes(),
        &16000u32.to_le_bytes(),
        &2u16.to_le_bytes(),
        &16u16.to_le_bytes(),
    ]
    .concat();
    let info = [&b"INFO"[..], &chunk(b"INAM", &[title.as_bytes(), b"\0"].concat())].concat();
    let body = [&b"WAVE"[..], &chunk(b"fmt ", &fmt), &chunk(b"data", &[0; 64]), &chunk(b"LIST", &info)].concat();
    chunk(b"RIFF", &body)
}

struct Sample {
    name: &'static str,
    kind: Kind,
    data: Vec<u8>,
    word: &'static str,
}

fn samples() -> Vec<Sample> {
    let long_html = format!("<p>{}</p>", "tapir ".repeat(MAX_TEXT / 3));
    let slide = format!("<a:t>{}</a:t>", "lemur ".repeat(MAX_TEXT / 12));
    let slides: Vec<(String, Vec<u8>)> =
        (1..=5).map(|i| (format!("ppt/slides/slide{i}.xml"), slide.clone().into_bytes())).collect();
    let slides: Vec<(&str, Vec<u8>)> = slides.iter().map(|(n, d)| (n.as_str(), d.clone())).collect();
    vec![
        Sample {
            name: "page.html",
            kind: Kind::Html,
            data: b"<script>var no;</script><p>caf&amp;e okapi</p>".to_vec(),
            word: "okapi",
        },
        Sample { name: "long.html", kind: Kind::Html, data: long_html.into_bytes(), word: "tapir" },
        Sample { name: "scan.pdf", kind: Kind::Pdf, data: pdf("wombat"), word: "wombat" },
        Sample {
            name: "report.docx",
            kind: Kind::Zip,
            data: zip(&[("word/document.xml", b"<w:p><w:t>pangolin</w:t></w:p>".to_vec())]),
            word: "pangolin",
        },
        Sample { name: "deck.pptx", kind: Kind::Zip, data: zip(&slides), word: "lemur" },
        Sample {
            name: "photo.jpg",
            kind: Kind::Photo,
            data: jpeg("Okapicam", "heron", "summer"),
            word: "Okapicam heron summer",
        },
        Sample { name: "song.wav", kind: Kind::Audio, data: wav("Narwhal Lullaby"), word: "Narwhal Lullaby" },
    ]
}

/// xorshift64: the same sequence on every run.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, n: usize) -> usize {
        usize::try_from(self.next() % u64::try_from(n.max(1)).unwrap()).unwrap()
    }
}

fn mutate(data: &[u8], rng: &mut Rng) -> Vec<u8> {
    let mut out = data.to_vec();
    match rng.below(4) {
        0 => out.truncate(rng.below(data.len() + 1)),
        1 => {
            for _ in 0..=rng.below(16) {
                let at = rng.below(out.len());
                out[at] = u8::try_from(rng.next() & 0xff).unwrap();
            }
        }
        2 => {
            let from = rng.below(out.len());
            let to = from + rng.below(out.len() - from);
            let copy = out[from..to].to_vec();
            let at = rng.below(out.len());
            out.splice(at..at, copy);
        }
        _ => out = (0..rng.below(4096)).map(|_| u8::try_from(rng.next() & 0xff).unwrap()).collect(),
    }
    out
}

fn extract_within(kind: Kind, path: &Path) -> Option<Vec<u8>> {
    let (send, receive) = mpsc::channel();
    let path = path.to_path_buf();
    std::thread::spawn(move || send.send(extract(kind, &path)).unwrap());
    receive.recv_timeout(Duration::from_secs(10)).unwrap_or_else(|_| panic!("extract hangs on {}", "input"))
}

#[test]
fn intact_samples_give_their_words_within_the_cap() {
    let dir = tempfile::tempdir().unwrap();
    for sample in samples() {
        let path = dir.path().join(sample.name);
        std::fs::write(&path, &sample.data).unwrap();
        let text = extract_within(sample.kind, &path).unwrap_or_else(|| panic!("{} unreadable", sample.name));
        let words = String::from_utf8_lossy(&text).split_whitespace().collect::<Vec<_>>().join(" ");
        assert!(words.contains(sample.word), "{}: {words:.200}", sample.name);
        assert!(text.len() <= MAX_TEXT, "{}: {} bytes", sample.name, text.len());
    }
}

#[test]
fn random_and_truncated_inputs_never_crash_hang_or_pass_the_cap() {
    let dir = tempfile::tempdir().unwrap();
    let samples = samples();
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    for round in 0..600 {
        let sample = &samples[round % samples.len()];
        let data = mutate(&sample.data, &mut rng);
        let path = dir.path().join(format!("{round}-{}", sample.name));
        std::fs::write(&path, &data).unwrap();
        if let Some(text) = extract_within(sample.kind, &path) {
            assert!(text.len() <= MAX_TEXT, "{}: {} bytes", path.display(), text.len());
        }
        std::fs::remove_file(&path).unwrap();
    }
}
