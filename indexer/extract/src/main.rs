//! andromeda-index-extract: reads one request per line on stdin, `<kind> <path>` (kind: html, pdf, zip, photo,
//! audio), and answers each with `<bytes>\n<text>`, or `-\n` when the file cannot be read. Runs as a child of the
//! index daemon inside the Extract sandbox: it reads files, and writes only to stdout.

use andromeda_index_extract::{Kind, extract};
use std::ffi::OsStr;
use std::io::{BufRead, Write};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

fn main() {
    if let Err(e) = andromeda_sandbox::enter(andromeda_sandbox::Profile::Extract) {
        eprintln!("andromeda-index-extract: sandbox: {e}");
        std::process::exit(1);
    }
    let mut stdin = std::io::stdin().lock();
    let mut stdout = std::io::stdout().lock();
    let mut line = Vec::new();
    loop {
        line.clear();
        match stdin.read_until(b'\n', &mut line) {
            Ok(0) | Err(_) => return,
            Ok(_) => {}
        }
        let request = line.strip_suffix(b"\n").unwrap_or(&line);
        let text = request.iter().position(|&b| b == b' ').and_then(|space| {
            let kind = Kind::from_word(&request[..space])?;
            extract(kind, Path::new(OsStr::from_bytes(&request[space + 1..])))
        });
        let written = match text {
            Some(text) => writeln!(stdout, "{}", text.len()).and_then(|()| stdout.write_all(&text)),
            None => stdout.write_all(b"-\n"),
        };
        if written.and_then(|()| stdout.flush()).is_err() {
            return;
        }
    }
}
