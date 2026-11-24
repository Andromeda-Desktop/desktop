//! Decoders the helper runs inside its sandbox. Each returns a sealed memfd the window maps read-only.

use crate::proto::{
    ERR_BAD_REQUEST, ERR_BINARY, ERR_DECODE, ERR_RANGE, ERR_TOO_LARGE, KIND_PDF, LINK_PAGE, LINK_URI, Msg, OP_FIND_REPLY, OP_INFO_REPLY, OP_LINKS_REPLY, OP_OUTLINE_REPLY,
    OP_PAGE, OP_SELECT_REPLY, OP_TEXT_REPLY, SELECT_GLYPH, SELECT_LINE, SELECT_WORD,
};
use rustix::fd::OwnedFd;
use rustix::fs::{MemfdFlags, SealFlags};
use std::fs::File;
use std::io::{Read, Seek, Write};

/// Text preview reads at most this much (docs/search.md §5).
pub const TEXT_LIMIT: usize = 1 << 20;
/// A NUL in this prefix marks the file as binary.
const BINARY_PROBE: usize = 8192;
pub const MAX_WIDTH: u32 = 8192;
pub const MAX_HEIGHT: u32 = 16384;
/// A tile is at most this many pixels on a side.
pub const MAX_TILE: u32 = 2048;
/// The widest page a tile may come from (8 x zoom of a wide page on a 2x display).
pub const MAX_TILE_SCALE_WIDTH: u32 = 65536;

/// Draws the `w` x `h` part at (`x`, `y`) of `page` at `scale` pixels per point into a sealed memfd.
fn draw(page: &poppler::Page, index: u32, scale: f64, x: u32, y: u32, w: u32, h: u32) -> Result<(Msg, OwnedFd), u32> {
    let wi = i32::try_from(w).map_err(|_| ERR_TOO_LARGE)?;
    let hi = i32::try_from(h).map_err(|_| ERR_TOO_LARGE)?;
    let mut surface = cairo::ImageSurface::create(cairo::Format::ARgb32, wi, hi).map_err(|_| ERR_DECODE)?;
    {
        let cr = cairo::Context::new(&surface).map_err(|_| ERR_DECODE)?;
        cr.set_source_rgb(1.0, 1.0, 1.0);
        cr.paint().map_err(|_| ERR_DECODE)?;
        cr.translate(-f64::from(x), -f64::from(y));
        cr.scale(scale, scale);
        page.render(&cr);
    }
    surface.flush();
    let stride = u32::try_from(surface.stride()).map_err(|_| ERR_DECODE)?;
    let data = surface.data().map_err(|_| ERR_DECODE)?;
    let fd = sealed("preview-page", &data)?;
    Ok((Msg::new(OP_PAGE, index, w, h, stride), fd))
}
/// Hits per page and selection rects per reply; a page with more is cut there.
pub const MAX_RECTS: usize = 4096;
/// The largest thumbnail side (freedesktop x-large is 512, xx-large 1024).
pub const MAX_THUMBNAIL: u32 = 1024;

pub(crate) fn sealed(name: &str, bytes: &[u8]) -> Result<OwnedFd, u32> {
    sealed_with(name, bytes, SealFlags::SHRINK | SealFlags::GROW | SealFlags::WRITE | SealFlags::SEAL)
}

/// A frame for wl_shm: the compositor maps a shm pool writable, which a write seal forbids. The size seals stay, so
/// the buffer cannot shrink under the compositor's mapping.
pub(crate) fn sealed_frame(name: &str, bytes: &[u8]) -> Result<OwnedFd, u32> {
    sealed_with(name, bytes, SealFlags::SHRINK | SealFlags::GROW | SealFlags::SEAL)
}

/// Writes a later frame of the same size into a released `sealed_frame` memfd.
pub(crate) fn rewrite_frame(fd: OwnedFd, bytes: &[u8]) -> Result<OwnedFd, u32> {
    let mut file = File::from(fd);
    file.rewind().map_err(|_| ERR_DECODE)?;
    file.write_all(bytes).map_err(|_| ERR_DECODE)?;
    Ok(OwnedFd::from(file))
}

fn sealed_with(name: &str, bytes: &[u8], seals: SealFlags) -> Result<OwnedFd, u32> {
    let fd = rustix::fs::memfd_create(name, MemfdFlags::CLOEXEC | MemfdFlags::ALLOW_SEALING).map_err(|_| ERR_DECODE)?;
    let mut file = File::from(fd);
    file.write_all(bytes).map_err(|_| ERR_DECODE)?;
    let fd = OwnedFd::from(file);
    rustix::fs::fcntl_add_seals(&fd, seals).map_err(|_| ERR_DECODE)?;
    Ok(fd)
}

pub struct Text {
    pub text: String,
    pub truncated: bool,
}

/// Reads the first `TEXT_LIMIT` bytes as UTF-8 (invalid sequences become U+FFFD).
///
/// # Errors
/// `ERR_BINARY` for a file with a NUL byte near its start, `ERR_DECODE` for a read error.
pub fn read_text(file: &mut File) -> Result<Text, u32> {
    let mut bytes = Vec::with_capacity(64 * 1024);
    file.rewind().map_err(|_| ERR_DECODE)?;
    file.take(TEXT_LIMIT as u64 + 1).read_to_end(&mut bytes).map_err(|_| ERR_DECODE)?;
    let truncated = bytes.len() > TEXT_LIMIT;
    bytes.truncate(TEXT_LIMIT);
    if bytes[..bytes.len().min(BINARY_PROBE)].contains(&0) {
        return Err(ERR_BINARY);
    }
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    // Lossy decoding grows each invalid byte to 3, and the limit may split the last character.
    let fits = text.floor_char_boundary(TEXT_LIMIT);
    let cut = truncated || fits < text.len();
    text.truncate(fits);
    if truncated && text.ends_with(char::REPLACEMENT_CHARACTER) {
        text.pop();
    }
    Ok(Text { text, truncated: cut })
}

/// # Errors
/// See [`read_text`].
pub fn text_reply(file: &mut File) -> Result<(Msg, OwnedFd), u32> {
    let text = read_text(file)?;
    let len = u32::try_from(text.text.len()).map_err(|_| ERR_TOO_LARGE)?;
    let fd = sealed("preview-text", text.text.as_bytes())?;
    Ok((Msg::new(OP_TEXT_REPLY, len, u32::from(text.truncated), 0, 0), fd))
}

pub struct Pdf {
    doc: poppler::Document,
    /// Annotations added in this session, by id - 1; None once removed.
    annots: Vec<Option<(u32, poppler::Annot)>>,
}

fn centi_points(v: f64) -> u32 {
    // Page sizes are positive and far below u32::MAX / 100 points.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let out = (v * 100.0).round().clamp(0.0, f64::from(u32::MAX)) as u32;
    out
}

#[allow(clippy::cast_possible_truncation)]
/// The 0-based page a link dest names, looking a named dest up in `doc`.
///
/// # Safety
/// `doc` is a live document and `dest` null or a dest of it.
#[allow(unsafe_code)]
unsafe fn dest_page(doc: *mut poppler::ffi::PopplerDocument, dest: *mut poppler::ffi::PopplerDest) -> Option<u32> {
    use poppler::ffi;
    if dest.is_null() {
        return None;
    }
    // SAFETY: the caller's contract; a dest found by name is ours to free.
    let page_num = unsafe {
        if (*dest).type_ == ffi::POPPLER_DEST_NAMED {
            if (*dest).named_dest.is_null() {
                return None;
            }
            let found = ffi::poppler_document_find_dest(doc, (*dest).named_dest);
            if found.is_null() {
                return None;
            }
            let n = (*found).page_num;
            ffi::poppler_dest_free(found);
            n
        } else {
            (*dest).page_num
        }
    };
    // poppler numbers pages from 1.
    u32::try_from(page_num).ok().and_then(|n| n.checked_sub(1))
}

/// Outline entries and nesting kept from a file; an outline past these is cut.
const MAX_OUTLINE: u32 = 4096;
const MAX_OUTLINE_DEPTH: u32 = 32;

#[derive(Default)]
struct Outline {
    rows: Vec<u8>,
    titles: Vec<u8>,
    count: u32,
}

impl Outline {
    /// Adds `iter` and its siblings after it, each followed by its children.
    #[allow(unsafe_code)]
    fn walk(&mut self, doc: *mut poppler::ffi::PopplerDocument, iter: &mut poppler::IndexIter, depth: u32) {
        use glib::translate::ToGlibPtrMut;
        use poppler::ffi;
        loop {
            if self.count >= MAX_OUTLINE {
                return;
            }
            let raw: *mut ffi::PopplerIndexIter = iter.to_glib_none_mut().0;
            // SAFETY: `raw` is a live iterator; the action it returns is ours to free, and its title and dest live
            // until then.
            unsafe {
                let action = ffi::poppler_index_iter_get_action(raw);
                if !action.is_null() {
                    let title = (*action).any.title;
                    let title = if title.is_null() { &[][..] } else { std::ffi::CStr::from_ptr(title).to_bytes() };
                    let page = if (*action).type_ == ffi::POPPLER_ACTION_GOTO_DEST {
                        dest_page(doc, (*action).goto_dest.dest)
                    } else {
                        None
                    };
                    if let Ok(len) = u32::try_from(title.len()) {
                        for v in [depth, page.unwrap_or(u32::MAX), len] {
                            self.rows.extend_from_slice(&v.to_le_bytes());
                        }
                        self.titles.extend_from_slice(title);
                        self.count += 1;
                    }
                    ffi::poppler_action_free(action);
                }
            }
            if depth < MAX_OUTLINE_DEPTH
                && let Some(mut child) = iter.child()
            {
                self.walk(doc, &mut child, depth + 1);
            }
            if !iter.next() {
                return;
            }
        }
    }
}

fn push_rect(out: &mut Vec<u8>, x0: f64, y0: f64, x1: f64, y1: f64) {
    for v in [x0, y0, x1, y1] {
        out.extend_from_slice(&(v as f32).to_le_bytes());
    }
}

impl Pdf {
    pub(crate) fn doc(&self) -> &poppler::Document {
        &self.doc
    }

    pub(crate) fn keep_annot(&mut self, page: u32, annot: poppler::Annot) -> u32 {
        self.annots.push(Some((page, annot)));
        u32::try_from(self.annots.len()).unwrap_or(u32::MAX)
    }

    pub(crate) fn take_annot(&mut self, id: u32) -> Option<(u32, poppler::Annot)> {
        self.annots.get_mut(usize::try_from(id).ok()?.checked_sub(1)?)?.take()
    }

    pub(crate) fn page(&self, index: u32) -> Result<poppler::Page, u32> {
        i32::try_from(index)
            .ok()
            .filter(|&i| i < self.doc.n_pages())
            .and_then(|i| self.doc.page(i))
            .ok_or(ERR_RANGE)
    }

    /// The links of page `index` that go to a page of this document or to a URI; no fd when there is none.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end.
    #[allow(unsafe_code)]
    pub fn links(&self, index: u32) -> Result<(Msg, Option<OwnedFd>), u32> {
        use glib::translate::ToGlibPtr;
        use poppler::ffi;
        let page = self.page(index)?;
        let (_, height) = page.size();
        let doc: *mut ffi::PopplerDocument = self.doc.to_glib_none().0;
        let mut records = Vec::new();
        let mut uris = Vec::new();
        for mapping in page.link_mapping().iter().take(MAX_RECTS) {
            let raw: *const ffi::PopplerLinkMapping = mapping.to_glib_none().0;
            // SAFETY: `mapping` owns a valid PopplerLinkMapping; its action, when set, is a PopplerAction whose
            // `type_` names the union member, and the dest and URI pointers it holds live as long as it.
            let target = unsafe {
                let action = (*raw).action;
                if action.is_null() {
                    continue;
                }
                match (*action).type_ {
                    ffi::POPPLER_ACTION_GOTO_DEST => dest_page(doc, (*action).goto_dest.dest).map(|p| (LINK_PAGE, p, None)),
                    ffi::POPPLER_ACTION_URI if !(*action).uri.uri.is_null() => {
                        let uri = std::ffi::CStr::from_ptr((*action).uri.uri).to_bytes().to_vec();
                        u32::try_from(uri.len()).ok().map(|n| (LINK_URI, n, Some(uri)))
                    }
                    _ => None,
                }
            };
            let Some((kind, value, uri)) = target else { continue };
            // SAFETY: as above.
            let area = unsafe { (*raw).area };
            // poppler reports areas with the origin at the bottom left of the page.
            push_rect(&mut records, area.x1, height - area.y2, area.x2, height - area.y1);
            records.extend_from_slice(&kind.to_le_bytes());
            records.extend_from_slice(&value.to_le_bytes());
            uris.extend(uri.unwrap_or_default());
        }
        let count = u32::try_from(records.len() / 24).unwrap_or(0);
        records.extend(uris);
        let fd = if count > 0 { Some(sealed("preview-links", &records)?) } else { None };
        Ok((Msg::new(OP_LINKS_REPLY, index, count, 0, 0), fd))
    }

    /// The document outline in reading order; no fd when there is none.
    ///
    /// # Errors
    /// `ERR_DECODE` when the reply cannot be written.
    #[allow(unsafe_code)]
    pub fn outline(&self) -> Result<(Msg, Option<OwnedFd>), u32> {
        use glib::translate::{FromGlibPtrFull, ToGlibPtr};
        let doc: *mut poppler::ffi::PopplerDocument = self.doc.to_glib_none().0;
        // SAFETY: `doc` is live; a null iterator (no outline) becomes None.
        let root: Option<poppler::IndexIter> =
            unsafe { FromGlibPtrFull::from_glib_full(poppler::ffi::poppler_index_iter_new(doc)) };
        let mut out = Outline::default();
        if let Some(mut iter) = root {
            out.walk(doc, &mut iter, 0);
        }
        let fd = if out.count > 0 {
            out.rows.extend(out.titles);
            Some(sealed("preview-outline", &out.rows)?)
        } else {
            None
        };
        Ok((Msg::new(OP_OUTLINE_REPLY, 0, out.count, 0, 0), fd))
    }

    /// The places of `query` on page `index`, top left origin; no fd when there is none.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end, `ERR_BAD_REQUEST` for an empty or non-UTF-8 query.
    pub fn find(&self, index: u32, query: &[u8]) -> Result<(Msg, Option<OwnedFd>), u32> {
        let query = std::str::from_utf8(query).ok().filter(|q| !q.trim().is_empty()).ok_or(ERR_BAD_REQUEST)?;
        let page = self.page(index)?;
        let (_, height) = page.size();
        let hits = page.find_text_with_options(
            query,
            poppler::FindFlags::DEFAULT | poppler::FindFlags::IGNORE_DIACRITICS | poppler::FindFlags::MULTILINE,
        );
        let mut rects = Vec::new();
        // poppler reports hits with the origin at the bottom left of the page.
        for hit in hits.iter().take(MAX_RECTS) {
            push_rect(&mut rects, hit.x1(), height - hit.y2(), hit.x2(), height - hit.y1());
        }
        let count = u32::try_from(rects.len() / 16).unwrap_or(0);
        let fd = if count > 0 { Some(sealed("preview-find", &rects)?) } else { None };
        Ok((Msg::new(OP_FIND_REPLY, index, count, 0, 0), fd))
    }

    /// The text inside `area` (4 f32 in points, top left origin) on page `index`, and the rects that cover it.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page past the end, `ERR_BAD_REQUEST` for a bad area or unit.
    pub fn select(&self, index: u32, unit: u32, area: &[u8]) -> Result<(Msg, Option<OwnedFd>), u32> {
        let style = match unit {
            SELECT_GLYPH => poppler::SelectionStyle::Glyph,
            SELECT_WORD => poppler::SelectionStyle::Word,
            SELECT_LINE => poppler::SelectionStyle::Line,
            _ => return Err(ERR_BAD_REQUEST),
        };
        let v: Vec<f64> = area
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| f64::from(f32::from_le_bytes(*c)))
            .collect();
        if area.len() != 16 || v.iter().any(|x| !x.is_finite()) {
            return Err(ERR_BAD_REQUEST);
        }
        let page = self.page(index)?;
        let mut rect = poppler::Rectangle::new();
        rect.set_x1(v[0]);
        rect.set_y1(v[1]);
        rect.set_x2(v[2]);
        rect.set_y2(v[3]);
        let text = page.selected_text(style, &mut rect).map(|t| t.to_string()).unwrap_or_default();
        // The region comes in device pixels at this scale; 10 keeps a tenth of a point.
        const SCALE: f64 = 10.0;
        let region = page.selected_region(SCALE, style, &mut rect);
        let mut out = Vec::new();
        let count = region.as_ref().map_or(0, |r| usize::try_from(r.num_rectangles()).unwrap_or(0)).min(MAX_RECTS);
        for i in 0..count {
            let Some(r) = region.as_ref().map(|r| r.rectangle(i32::try_from(i).unwrap_or(0))) else { break };
            let (x, y) = (f64::from(r.x()) / SCALE, f64::from(r.y()) / SCALE);
            push_rect(&mut out, x, y, x + f64::from(r.width()) / SCALE, y + f64::from(r.height()) / SCALE);
        }
        out.extend_from_slice(text.as_bytes());
        let len = u32::try_from(text.len()).map_err(|_| ERR_TOO_LARGE)?;
        // An empty selection has no fd: the window cannot map an empty file.
        let fd = if out.is_empty() { None } else { Some(sealed("preview-select", &out)?) };
        Ok((Msg::new(OP_SELECT_REPLY, len, u32::try_from(count).unwrap_or(0), 0, 0), fd))
    }

    /// # Errors
    /// `ERR_DECODE` when poppler cannot parse the file (encrypted files included).
    pub fn open(file: File) -> Result<Self, u32> {
        use std::os::fd::IntoRawFd;
        let doc = poppler::Document::from_fd(file.into_raw_fd(), None).map_err(|_| ERR_DECODE)?;
        Ok(Self { doc, annots: Vec::new() })
    }

    #[must_use]
    pub fn info(&self) -> Msg {
        let pages = u32::try_from(self.doc.n_pages()).unwrap_or(0);
        let (w, h) = self.doc.page(0).map_or((0.0, 0.0), |p| p.size());
        Msg::new(OP_INFO_REPLY, KIND_PDF, pages, centi_points(w), centi_points(h))
    }

    /// Renders one page `width` pixels wide on white, at most `max_height` pixels high.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page index past the end, `ERR_TOO_LARGE` for a size over the caps.
    pub fn render(&self, index: u32, width: u32, max_height: u32) -> Result<(Msg, OwnedFd), u32> {
        let page = self.page(index)?;
        let (pw, ph) = page.size();
        if width == 0 || width > MAX_WIDTH || pw <= 0.0 || ph <= 0.0 {
            return Err(ERR_TOO_LARGE);
        }
        let scale = f64::from(width) / pw;
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let height = (ph * scale).ceil().max(1.0) as u32;
        if height > MAX_HEIGHT.min(max_height.max(1)) {
            return Err(ERR_TOO_LARGE);
        }
        draw(&page, index, scale, 0, 0, width, height)
    }

    /// Renders the `w` x `h` pixel part at (`x`, `y`) of the page drawn `page_width` pixels wide, on white.
    ///
    /// # Errors
    /// `ERR_RANGE` for a page index past the end, `ERR_TOO_LARGE` for a tile or scale over the caps.
    pub fn render_tile(&self, index: u32, page_width: u32, x: u32, y: u32, w: u32, h: u32) -> Result<(Msg, OwnedFd), u32> {
        let page = self.page(index)?;
        let (pw, ph) = page.size();
        if page_width == 0 || page_width > MAX_TILE_SCALE_WIDTH || pw <= 0.0 || ph <= 0.0 {
            return Err(ERR_TOO_LARGE);
        }
        if w == 0 || h == 0 || w > MAX_TILE || h > MAX_TILE {
            return Err(ERR_TOO_LARGE);
        }
        let scale = f64::from(page_width) / pw;
        if f64::from(x) >= f64::from(page_width) || f64::from(y) >= ph * scale {
            return Err(ERR_RANGE);
        }
        draw(&page, index, scale, x, y, w, h)
    }

    /// The first page fitted in a `size` x `size` box on white, as PNG (the freedesktop thumbnailer contract).
    ///
    /// # Errors
    /// `ERR_RANGE` without pages, `ERR_TOO_LARGE` for a size over `MAX_THUMBNAIL`, `ERR_DECODE` when writing fails.
    pub fn thumbnail_png(&self, size: u32, out: &mut impl std::io::Write) -> Result<(), u32> {
        let page = self.page(0)?;
        let (pw, ph) = page.size();
        if size == 0 || size > MAX_THUMBNAIL || pw <= 0.0 || ph <= 0.0 {
            return Err(ERR_TOO_LARGE);
        }
        let scale = f64::from(size) / pw.max(ph);
        #[allow(clippy::cast_possible_truncation)]
        let (w, h) = (((pw * scale).round() as i32).max(1), ((ph * scale).round() as i32).max(1));
        let surface = cairo::ImageSurface::create(cairo::Format::Rgb24, w, h).map_err(|_| ERR_DECODE)?;
        {
            let cr = cairo::Context::new(&surface).map_err(|_| ERR_DECODE)?;
            cr.set_source_rgb(1.0, 1.0, 1.0);
            cr.paint().map_err(|_| ERR_DECODE)?;
            cr.scale(scale, scale);
            page.render(&cr);
        }
        surface.write_to_png(out).map_err(|_| ERR_DECODE)
    }
}
