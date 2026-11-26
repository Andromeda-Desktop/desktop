#include "viewer_window.h"
#include "core/input/pointer_axis.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "app/command_table.h"
#include "app/state_store.h"
#include "share/link_share.h"
#include "share/share_menu.h"
#include "core/text_clipboard.h"
#include "core/input/cursor_shape.h"
#include "ui/controls/alert.h"
#include "ui/controls/input.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <glib.h>
#include <charconv>
#include <cstring>
#include <array>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace viewer {

  namespace {
    constexpr Logger kLog("viewer");
    constexpr std::size_t kMaxResultRows = 200;
    constexpr float kThumbWidth = 104.0F;
    constexpr float kPagePadding = 24.0F;
    constexpr std::uint32_t kMaxRenderWidth = 8192;
    // Device px: a whole page renders at most this wide; past it, kTile x kTile tiles show the part in view.
    constexpr std::uint32_t kBaseWidth = 2048;
    constexpr std::uint32_t kTile = 512;
    constexpr std::uint32_t kInkRgb = 0x1f5fd6;
    constexpr std::uint32_t kNoteRgb = 0xffd400;
    constexpr float kStampHandle = 12.0F; // logical px: the resize handle's side, and its grab reach
    constexpr float kStampMinWidth = 24.0F; // points
    constexpr float kInkWidth = 2.0F; // points: the helper's INK_WIDTH
    constexpr std::uint32_t kMaxPageHeight = 16384;
    constexpr std::uint32_t kImageBox = 8192;
    // proto MAX_PAYLOAD: a longer request ends the helper.
    constexpr std::size_t kMaxQuery = 256;

    std::string contentTypeOf(const std::filesystem::path& path) {
      GFile* file = g_file_new_for_path(path.c_str());
      GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE,
                                          nullptr, nullptr);
      std::string type;
      if (info != nullptr) {
        if (const char* t = g_file_info_get_content_type(info); t != nullptr) {
          type = t;
        }
        g_object_unref(info);
      }
      g_object_unref(file);
      return type;
    }

    // Cuts at a UTF-8 character boundary.
    std::string capQuery(std::string query) {
      if (query.size() <= kMaxQuery) {
        return query;
      }
      std::size_t end = kMaxQuery;
      while (end > 0 && (static_cast<unsigned char>(query[end]) & 0xC0U) == 0x80U) {
        --end;
      }
      query.resize(end);
      return query;
    }

    std::vector<RectF> readRects(const std::uint8_t* bytes, std::size_t count) {
      std::vector<RectF> rects(count);
      std::memcpy(rects.data(), bytes, count * sizeof(RectF));
      return rects;
    }

    // One line of the hit's text: whitespace runs collapse, and a hit far into the line starts the text near it.
    std::string snippetAround(const std::string& line, const std::string& query) {
      std::string text;
      for (const char c : line) {
        const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (!space || (!text.empty() && text.back() != ' ')) {
          text.push_back(space ? ' ' : c);
        }
      }
      const auto lower = [](std::string v) {
        std::ranges::transform(v, v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
      };
      constexpr std::size_t kLead = 24; // bytes of context before the hit
      const std::size_t at = lower(text).find(lower(query));
      if (at != std::string::npos && at > kLead) {
        std::size_t from = text.find(' ', at - kLead);
        from = from == std::string::npos || from >= at ? at - kLead : from + 1;
        while (from > 0 && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) {
          --from; // a UTF-8 continuation byte: start at its lead byte
        }
        text = "…" + text.substr(from);
      }
      return text;
    }

    // Pango markup of a snippet: each case-insensitive match of `query` on an accent wash, as on the page.
    std::string hitMarkup(std::string_view text, const std::string& query, bool current) {
      const auto escape = [](std::string& out, std::string_view part) {
        for (const char c : part) {
          out += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
        }
      };
      std::string lower(text);
      std::string needle(query);
      for (std::string* v : {&lower, &needle}) {
        std::ranges::transform(*v, v->begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      }
      const Color c = colorForRole(current ? ColorRole::TermFindHitCurrent : ColorRole::TermFindHit);
      const auto byte = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      const Color ink = colorForRole(ColorRole::Label1);
      const std::string open = std::format(
          R"(<span background="#{:02x}{:02x}{:02x}" bgalpha="{}%" foreground="#{:02x}{:02x}{:02x}" fgalpha="{}%">)",
          byte(c.r), byte(c.g), byte(c.b), std::max(1L, std::lround(c.a * 100.0F)), byte(ink.r), byte(ink.g),
          byte(ink.b), std::max(1L, std::lround(ink.a * 100.0F)));
      std::string out;
      std::size_t from = 0;
      for (std::size_t at = needle.empty() ? std::string::npos : lower.find(needle); at != std::string::npos;
           at = lower.find(needle, from)) {
        escape(out, text.substr(from, at - from));
        out += open;
        escape(out, text.substr(at, needle.size()));
        out += "</span>";
        from = at + needle.size();
      }
      escape(out, text.substr(from));
      return out;
    }

    std::vector<std::uint8_t> areaPayload(float x0, float y0, float x1, float y1) {
      std::vector<std::uint8_t> out(16);
      const float v[4] = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
      std::memcpy(out.data(), v, sizeof(v));
      return out;
    }
  } // namespace

  ViewerWindow::ViewerWindow(AppLoop& loop, AppWindow& window, StateStore& state, RenderBudget& budget)
      : m_loop(loop), m_window(window), m_state(state), m_budget(budget) {}

  void ViewerWindow::takeJob() {
    ++m_inFlight;
    ++m_budget.inFlight;
  }

  void ViewerWindow::endJob() {
    --m_inFlight;
    --m_budget.inFlight;
    if (m_budget.released) {
      m_budget.released(this);
    }
  }

  void ViewerWindow::releaseJobs() {
    m_budget.inFlight -= std::exchange(m_inFlight, 0);
    if (m_budget.released) {
      m_budget.released(this);
    }
  }

  // A rebuilt ScrollView clamps its offset to its content, which is empty until the first layout: restore after it.
  void ViewerWindow::onLayout() {
    // Once per rebuild: a document shorter than the old offset clamps, and must not loop.
    // A placed note's field takes the keys at once.
    if (m_focusNote && m_noteInput != nullptr) {
      m_focusNote = false;
      m_window.input().setFocus(m_noteInput->inputArea());
    }
    // Fit page follows the view height, known only after layout.
    bool rebuilding = false;
    if (m_scroll != nullptr && m_model.fit() == DocumentModel::Fit::Page &&
        m_model.setViewportHeight(m_scroll->height() - 2.0F * kPagePadding * m_window.uiScale())) {
      rebuildContent();
      rebuilding = true;
    }
    // The remembered page: its box is final once no rebuild for a new fit is pending.
    if (m_restorePage && m_scroll != nullptr && !rebuilding) {
      m_scrollOffset = m_model.pageBox(*m_restorePage).y0;
      m_restorePage.reset();
      m_restoreScroll = true;
    }
    if (m_restoreScroll && m_scroll != nullptr) {
      m_restoreScroll = false;
      if (m_scroll->scrollOffset() != m_scrollOffset) {
        m_scroll->setScrollOffset(m_scrollOffset);
        m_window.invalidate();
      }
    }
  }

  ViewerWindow::~ViewerWindow() {
    if (unsaved()) {
      kLog.info("unsaved edits dropped: {}", m_path.string());
    }
    m_decoder.cancel();
    dropHelper();
    m_budget.inFlight -= m_inFlight;
  }

  void ViewerWindow::dropHelper() {
    if (m_helper) {
      m_loop.removeSource(m_helper.get());
      m_helper.reset();
    }
  }

  void ViewerWindow::open(const std::filesystem::path& path) {
    ++m_generation;
    m_decoder.cancel();
    dropHelper();
    m_edits.clear();
    m_undone.clear();
    m_stamp.reset();
    m_savedDepth = 0;
    m_notePoint.reset();
    m_signing = false;
    m_drawing = false;
    m_path = path;
    for (Page& page : m_pages) {
      dropPage(page);
    }
    for (Page& page : m_thumbs) {
      dropPage(page);
    }
    m_pages.clear();
    m_thumbs.clear();
    for (auto& [key, tile] : m_tiles) {
      dropTile(tile);
    }
    m_tiles.clear();
    m_jobs.clear();
    releaseJobs();
    m_pendingInk.clear();
    m_image = {};
    m_selection.reset();
    m_searchText.clear();
    m_model.clearSearch();
    m_scrollOffset = 0.0F;
    m_restorePage.reset();
    m_savedView.reset();
    m_hOffset = 0.0F;
    m_error.clear();
    updateTitle();
    const auto kind = preview::kindForContentType(contentTypeOf(path));
    if (kind == preview::FileKind::Pdf) {
      openPdf();
    } else if (kind == preview::FileKind::Image) {
      loadFolder();
      openImage();
    } else {
      fail(i18n::tr("viewer.errors.unsupported"));
    }
    rebuild();
  }

  void ViewerWindow::fail(std::string reason) {
    m_mode = Mode::Error;
    m_error = std::move(reason);
    rebuild();
  }

  void ViewerWindow::openPdf() {
    m_mode = Mode::Loading;
    m_helper = preview::HelperClient::start(preview::HelperKind::Pdf, m_path.string());
    if (!m_helper) {
      fail(i18n::tr("viewer.errors.file-open"));
      return;
    }
    m_loop.addSource(m_helper.get());
    m_helper->request({.op = preview::op::Info}, [this, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      if (reply.msg.op != preview::op::InfoReply || reply.msg.b == 0) {
        DeferredCall::callLaterIfAlive(m_alive, [this, generation] {
          if (generation == m_generation) {
            dropHelper();
          }
        });
        fail(i18n::tr("viewer.errors.file-open"));
        return;
      }
      m_model.reset(std::min<std::uint32_t>(reply.msg.b, 100000), static_cast<float>(reply.msg.c) / 100.0F,
                    static_cast<float>(reply.msg.d) / 100.0F);
      m_pages.assign(m_model.pageCount(), {});
      m_thumbs.assign(m_model.pageCount(), {});
      m_links.assign(m_model.pageCount(), {});
      m_outline.clear();
      m_mode = Mode::Pdf;
      restoreViewState();
      requestOutline();
      kLog.info("opened pdf {} pages", m_model.pageCount());
      rebuild();
    });
  }

  void ViewerWindow::loadFolder() {
    m_folder.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_path.parent_path(), ec)) {
      if (entry.is_regular_file(ec) && DirectoryScanner::isImagePath(entry.path())) {
        m_folder.push_back(entry.path());
      }
    }
    std::ranges::sort(m_folder);
    const auto it = std::ranges::find(m_folder, m_path);
    m_folderIndex = it != m_folder.end() ? static_cast<std::size_t>(it - m_folder.begin()) : 0;
  }

  void ViewerWindow::openImage() {
    m_mode = Mode::Loading;
    m_decoder.request(m_path.string(), kImageBox, kImageBox, [this, generation = m_generation](preview::Pixels pixels) {
      if (generation != m_generation) {
        return;
      }
      if (pixels.width == 0) {
        fail(i18n::tr("viewer.errors.image-open"));
        return;
      }
      m_image = std::move(pixels);
      m_mode = Mode::Image;
      kLog.info("opened image {}x{}", m_image.width, m_image.height);
      rebuild();
    });
  }

  void ViewerWindow::step(int direction) {
    // An image that failed to decode is still a place in its folder: Left and Right move on from it.
    if (m_mode == Mode::Image || ((m_mode == Mode::Loading || m_mode == Mode::Error) && !m_folder.empty() && !m_helper)) {
      if (m_folder.size() < 2) {
        return;
      }
      const auto n = static_cast<long>(m_folder.size());
      const long next = (static_cast<long>(m_folderIndex) + direction + n) % n;
      const auto path = m_folder[static_cast<std::size_t>(next)];
      ++m_generation;
      m_decoder.cancel();
      m_path = path;
      m_folderIndex = static_cast<std::size_t>(next);
      m_error.clear();
      updateTitle();
      openImage();
      rebuild();
      return;
    }
    if (m_mode == Mode::Pdf && m_scroll != nullptr) {
      const std::size_t page = currentPage();
      const std::size_t target = direction < 0 ? (page == 0 ? 0 : page - 1) : std::min(page + 1, m_model.pageCount() - 1);
      m_scroll->setScrollOffset(m_model.pageBox(target).y0);
      onScroll();
    }
  }

  void ViewerWindow::rebuild() { m_window.requestRebuild(); }

  void ViewerWindow::rebuildContent() {
    if (m_contentRebuild) {
      m_contentRebuild();
    } else {
      rebuild();
    }
  }

  namespace {
    constexpr std::size_t kRememberedDocuments = 50;

    std::string documentKey(const std::filesystem::path& path) {
      std::error_code ec;
      const std::string full = std::filesystem::weakly_canonical(path, ec).string();
      gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, ec ? path.c_str() : full.c_str(), -1);
      std::string key(sum);
      g_free(sum);
      return key;
    }

    const char* fitName(DocumentModel::Fit fit) {
      switch (fit) {
      case DocumentModel::Fit::None:
        return "none";
      case DocumentModel::Fit::Page:
        return "page";
      case DocumentModel::Fit::Width:
        break;
      }
      return "width";
    }
  } // namespace

  void ViewerWindow::restoreViewState() {
    m_documentKey = documentKey(m_path);
    const auto docs = m_state.get<nlohmann::json>("/viewer/documents", nlohmann::json::object());
    if (!docs.is_object()) {
      return;
    }
    const auto it = docs.find(m_documentKey);
    if (it == docs.end() || !it->is_object()) {
      return;
    }
    std::string fit;
    float zoom = 1.0F;
    std::size_t page = 0;
    try {
      fit = it->value("fit", std::string("width"));
      zoom = it->value("zoom", 1.0F);
      page = it->value("page", std::size_t{0});
    } catch (const nlohmann::json::exception&) {
      kLog.warn("bad view state for {}", m_path.string());
      return;
    }
    if (fit == "page") {
      m_model.fitPage();
    } else if (fit == "none") {
      m_model.setZoom(zoom);
    }
    if (page > 0 && page < m_model.pageCount()) {
      m_restorePage = page;
    }
  }

  void ViewerWindow::saveViewState() {
    if (m_mode != Mode::Pdf || m_scroll == nullptr || m_restorePage) {
      return;
    }
    const ViewState view{.page = currentPage(), .fit = m_model.fit(), .zoom = m_model.zoom()};
    if (m_savedView == view) {
      return;
    }
    m_savedView = view;
    auto docs = m_state.get<nlohmann::json>("/viewer/documents", nlohmann::json::object());
    if (!docs.is_object()) {
      docs = nlohmann::json::object();
    }
    docs[m_documentKey] = {{"page", view.page}, {"fit", fitName(view.fit)}, {"zoom", view.zoom},
                                 {"time", static_cast<std::int64_t>(std::time(nullptr))}};
    while (docs.size() > kRememberedDocuments) {
      const auto time = [](const nlohmann::json& entry) {
        return entry.is_object() ? entry.value("time", std::int64_t{0}) : std::int64_t{0};
      };
      auto oldest = docs.begin();
      for (auto entry = docs.begin(); entry != docs.end(); ++entry) {
        if (time(*entry) < time(*oldest)) {
          oldest = entry;
        }
      }
      docs.erase(oldest);
    }
    m_state.set("/viewer/documents", std::move(docs));
  }

  void ViewerWindow::goToPage(std::size_t page) {
    if (m_mode != Mode::Pdf || m_scroll == nullptr || m_model.pageCount() == 0) {
      return;
    }
    m_scroll->setScrollOffset(m_model.pageBox(std::min(page, m_model.pageCount() - 1)).y0);
    onScroll();
  }

  std::size_t ViewerWindow::currentPage() const {
    return m_scroll != nullptr ? m_model.pageAt(m_scroll->scrollOffset() + 1.0F) : 0;
  }

  void ViewerWindow::onScroll() {
    if (m_scroll != nullptr) {
      m_scrollOffset = m_scroll->scrollOffset();
    }
    saveViewState();
    requestVisiblePages();
    refreshLabels();
  }

  void ViewerWindow::requestLinks(std::size_t page) {
    if (page >= m_links.size() || m_links[page].state != PageLinks::State::Unasked) {
      return;
    }
    m_links[page].state = PageLinks::State::Asked;
    m_helper->request({.op = preview::op::Links, .a = static_cast<std::uint32_t>(page)}, {},
                      [this, page, generation = m_generation](const preview::HelperReply& reply) {
                        if (generation != m_generation || page >= m_links.size()) {
                          return;
                        }
                        PageLinks& out = m_links[page];
                        out.state = PageLinks::State::Known;
                        if (reply.msg.op != preview::op::LinksReply || reply.data == nullptr) {
                          return;
                        }
                        const auto bytes = reply.data->bytes();
                        constexpr std::size_t kRecord = sizeof(RectF) + 2 * sizeof(std::uint32_t);
                        const std::size_t count = reply.msg.b;
                        std::size_t text = count * kRecord;
                        if (text > bytes.size()) {
                          return;
                        }
                        for (std::size_t r = 0; r < count; ++r) {
                          const std::uint8_t* at = bytes.data() + r * kRecord;
                          Link link;
                          std::uint32_t kind = 0;
                          std::uint32_t value = 0;
                          std::memcpy(&link.area, at, sizeof(RectF));
                          std::memcpy(&kind, at + sizeof(RectF), sizeof(kind));
                          std::memcpy(&value, at + sizeof(RectF) + sizeof(kind), sizeof(value));
                          if (kind == preview::kLinkPage) {
                            link.target = static_cast<std::size_t>(value);
                          } else if (kind == preview::kLinkUri && value <= bytes.size() - text) {
                            link.target = std::string(reinterpret_cast<const char*>(bytes.data()) + text, value);
                            text += value;
                          } else {
                            continue;
                          }
                          out.links.push_back(std::move(link));
                        }
                      });
  }

  const ViewerWindow::Link* ViewerWindow::linkAt(std::size_t page, float x, float y) const {
    if (page >= m_links.size()) {
      return nullptr;
    }
    for (const Link& link : m_links[page].links) {
      if (x >= link.area.x0 && x <= link.area.x1 && y >= link.area.y0 && y <= link.area.y1) {
        return &link;
      }
    }
    return nullptr;
  }

  void ViewerWindow::follow(const Link& link) {
    if (const auto* page = std::get_if<std::size_t>(&link.target)) {
      goToPage(*page);
      return;
    }
    // The URI comes from the file: open only web and mail links, never a local file or a program.
    const std::string& uri = std::get<std::string>(link.target);
    if (!uri.starts_with("https://") && !uri.starts_with("http://") && !uri.starts_with("mailto:")) {
      kLog.info("link not opened: {}", uri);
      return;
    }
    std::unique_ptr<GAppLaunchContext, void (*)(gpointer)> context(g_app_launch_context_new(), g_object_unref);
    if (const std::string token = m_loop.wayland().requestActivationToken(m_window.wlSurface()); !token.empty()) {
      g_app_launch_context_setenv(context.get(), "XDG_ACTIVATION_TOKEN", token.c_str());
    }
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(uri.c_str(), context.get(), &error)) {
      kLog.warn("open {}: {}", uri, error != nullptr ? error->message : "?");
      g_clear_error(&error);
    }
  }

  void ViewerWindow::requestVisiblePages() {
    if (m_mode != Mode::Pdf || !m_helper || m_scroll == nullptr) {
      return;
    }
    // The queue holds what the view needs now, most needed first; what scrolled away is never sent.
    m_jobs.clear();
    const float top = m_scroll->scrollOffset();
    auto [first, last] = m_model.visiblePages(top, top + m_scroll->height() * 1.5F);
    const float scale = m_window.uiScale();
    // Keep a few rendered pages around the view; a long document must not hold every page in memory.
    constexpr std::size_t kKeep = 3;
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      if ((i + kKeep < first || i > last + kKeep) && m_pages[i].requestedWidth != 0) {
        dropPage(m_pages[i]);
        if (i < m_pageImages.size() && m_pageImages[i] != nullptr) {
          m_window.updateInPlace([&](Renderer& renderer) { m_pageImages[i]->setExternalTexture(renderer, {}); });
        }
      }
    }
    for (std::size_t i = first; i <= last && i < m_pages.size(); ++i) {
      requestLinks(i);
      const RectF box = m_model.pageBox(i);
      // Past kBaseWidth the whole page stays at that width and tiles carry the detail in view.
      const auto width = std::min(kBaseWidth, static_cast<std::uint32_t>(std::lround((box.x1 - box.x0) * scale)));
      // Render again only when the zoom changed the width noticeably.
      if (m_pages[i].requestedWidth != 0 && std::abs(static_cast<float>(m_pages[i].requestedWidth) - static_cast<float>(width))
                                                < 0.1F * static_cast<float>(width)) {
        continue;
      }
      m_jobs.push_back({.kind = Job::Kind::Page, .page = i, .width = width});
    }
    queueTiles();
    queueThumbs();
    pump();
  }

  void ViewerWindow::queueTiles() {
    if (m_hscroll == nullptr) {
      return;
    }
    const float s = m_window.uiScale();
    const float pad = kPagePadding * s;
    const float top = m_scroll->scrollOffset() - pad;
    const float left = m_hscroll->scrollOffset() - pad;
    const float viewW = m_hscroll->width();
    const float viewH = m_scroll->height();
    const float column = std::max(m_hscroll->width() - 2.0F * pad, widestPage());
    std::vector<std::uint64_t> keep;
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = box.x1 - box.x0;
      const auto pageWidth = static_cast<std::uint32_t>(std::lround(w * s));
      if (pageWidth <= kBaseWidth || box.y1 < top - viewH || box.y0 > top + 2.0F * viewH) {
        continue;
      }
      const float px = pageWidth / w; // device px per logical px
      const float pageX = (column - w) / 2.0F;
      const auto pageHeight = static_cast<std::uint32_t>(std::lround((box.y1 - box.y0) * px));
      // The view plus one tile around it, in device px of this page.
      const float x0 = std::max(0.0F, (left - pageX) * px - kTile);
      const float x1 = std::min(static_cast<float>(pageWidth), (left + viewW - pageX) * px + kTile);
      const float y0 = std::max(0.0F, (top - box.y0) * px - kTile);
      const float y1 = std::min(static_cast<float>(pageHeight), (top + viewH - box.y0) * px + kTile);
      for (auto ty = static_cast<std::uint32_t>(y0 / kTile); static_cast<float>(ty * kTile) < y1; ++ty) {
        for (auto tx = static_cast<std::uint32_t>(x0 / kTile); static_cast<float>(tx * kTile) < x1; ++tx) {
          const std::uint64_t key = tileKey(i, tx, ty);
          keep.push_back(key);
          if (const auto it = m_tiles.find(key); it != m_tiles.end() && it->second.pageWidth == pageWidth) {
            continue;
          }
          m_jobs.push_back({.kind = Job::Kind::Tile, .page = i, .width = pageWidth, .x = tx * kTile, .y = ty * kTile,
                            .w = std::min(kTile, pageWidth - tx * kTile), .h = std::min(kTile, pageHeight - ty * kTile),
                            .key = key});
        }
      }
    }
    std::ranges::sort(keep);
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
      if (!std::ranges::binary_search(keep, it->first)) {
        dropTile(it->second);
        it = m_tiles.erase(it);
      } else {
        ++it;
      }
    }
  }

  void ViewerWindow::queueThumbs() {
    if (m_thumbScroll == nullptr) {
      return;
    }
    const float s = m_window.uiScale();
    const float view = m_window.contentRect().height;
    const float top = m_thumbScroll->scrollOffset() - view;
    const float bottom = m_thumbScroll->scrollOffset() + 2.0F * view;
    const auto width = static_cast<std::uint32_t>(std::lround(kThumbWidth * s));
    float y = Style::spaceMd * s;
    for (std::size_t i = 0; i < m_thumbs.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float h = kThumbWidth * s * (box.y1 - box.y0) / std::max(1.0F, box.x1 - box.x0);
      if (y + h < top || y > bottom) {
        if (m_thumbs[i].requestedWidth != 0) {
          dropPage(m_thumbs[i]);
          if (i < m_thumbImages.size() && m_thumbImages[i] != nullptr) {
            m_window.updateInPlace([&](Renderer& renderer) { m_thumbImages[i]->setExternalTexture(renderer, {}); });
          }
        }
      } else if (m_thumbs[i].requestedWidth == 0) {
        m_jobs.push_back({.kind = Job::Kind::Thumb, .page = i, .width = width});
      }
      y += h + Style::spaceMd * s;
    }
  }

  void ViewerWindow::requestThumbs() {
    if (m_mode != Mode::Pdf || !m_helper) {
      return;
    }
    std::erase_if(m_jobs, [](const Job& job) { return job.kind == Job::Kind::Thumb; });
    queueThumbs();
    pump();
  }

  void ViewerWindow::pump() {
    // Two in flight per window, shared by its panes: a helper works on one while the next waits, and a scroll never
    // queues stale work behind it.
    constexpr int kInFlight = 2;
    while (m_budget.inFlight < kInFlight && !m_jobs.empty() && m_helper) {
      const Job job = m_jobs.front();
      m_jobs.erase(m_jobs.begin());
      if (job.kind == Job::Kind::Tile) {
        if (const auto it = m_tiles.find(job.key); it != m_tiles.end() && it->second.pageWidth == job.width) {
          continue;
        }
        requestTile(job);
      } else {
        std::vector<Page>& cache = job.kind == Job::Kind::Page ? m_pages : m_thumbs;
        if (job.page >= cache.size() || cache[job.page].requestedWidth == job.width) {
          continue;
        }
        requestRender(cache, job.page, job.width);
      }
    }
  }

  std::uint64_t ViewerWindow::tileKey(std::size_t page, std::uint32_t tx, std::uint32_t ty) {
    return static_cast<std::uint64_t>(page) << 32U | static_cast<std::uint64_t>(ty) << 16U | tx;
  }

  void ViewerWindow::dropTile(Tile& tile) {
    if (tile.px.texture.valid()) {
      m_dropped.push_back(tile.px.texture);
    }
    if (tile.node != nullptr && tile.node->parent() != nullptr) {
      (void)tile.node->parent()->removeChild(tile.node);
      m_window.invalidate();
    }
    tile = {};
  }

  void ViewerWindow::requestTile(const Job& job) {
    // An existing tile (an edit made it stale) stays shown until its new pixels land.
    Tile& tile = m_tiles[job.key];
    tile.page = job.page;
    tile.pageWidth = job.width;
    tile.x = job.x;
    tile.y = job.y;
    takeJob();
    preview::HelperMsg msg{.op = preview::op::RenderTile, .a = static_cast<std::uint32_t>(job.page), .b = job.width,
                           .c = job.x, .d = job.y, .e = job.w << 16U | job.h};
    m_helper->request(msg, [this, job, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      endJob();
      const auto it = m_tiles.find(job.key);
      const auto& m = reply.msg;
      const bool valid = m.op == preview::op::Page && reply.data != nullptr && m.b == job.w && m.c == job.h
          && m.b <= m.d / 4 && static_cast<std::uint64_t>(m.d) * m.c <= reply.data->bytes().size();
      if (it != m_tiles.end() && it->second.pageWidth == job.width && valid) {
        Tile& tile = it->second;
        tile.px.pixels = reply.data;
        tile.px.width = m.b;
        tile.px.height = m.c;
        tile.px.stride = m.d;
        showTile(tile);
        kLog.debug("tile page {} at {},{} of width {}", job.page + 1, job.x, job.y, job.width);
      }
      pump();
    });
  }

  void ViewerWindow::showTile(Tile& tile) {
    if (tile.page >= m_tileLayers.size() || m_tileLayers[tile.page] == nullptr) {
      return; // the next build places it
    }
    m_window.updateInPlace([&](Renderer& renderer) {
      for (TextureHandle& texture : m_dropped) {
        renderer.textureManager().unload(texture);
      }
      m_dropped.clear();
      const TextureHandle texture = pageTexture(renderer, tile.px);
      if (tile.node == nullptr) {
        tile.node = placeTile(tile);
      }
      if (tile.node != nullptr) {
        tile.node->setExternalTexture(renderer, texture);
      }
    });
  }

  Image* ViewerWindow::placeTile(const Tile& tile) {
    Node* layer = m_tileLayers[tile.page];
    const RectF box = m_model.pageBox(tile.page);
    const float f = (box.x1 - box.x0) / static_cast<float>(tile.pageWidth); // logical px per tile px
    auto image = ui::image({.fit = ImageFit::Stretch, .width = static_cast<float>(tile.px.width) * f,
                            .height = static_cast<float>(tile.px.height) * f});
    image->setParticipatesInLayout(false);
    image->setPosition(static_cast<float>(tile.x) * f, static_cast<float>(tile.y) * f);
    return static_cast<Image*>(layer->addChild(std::move(image)));
  }

  void ViewerWindow::dropPage(Page& page) {
    if (page.texture.valid()) {
      m_dropped.push_back(page.texture);
    }
    page = {};
  }

  void ViewerWindow::showRender(std::vector<Page>& cache, std::size_t page) {
    std::vector<Image*>& images = &cache == &m_pages ? m_pageImages : m_thumbImages;
    if (page >= images.size() || images[page] == nullptr) {
      return; // the next build uploads it
    }
    m_window.updateInPlace([&](Renderer& renderer) {
      for (TextureHandle& texture : m_dropped) {
        renderer.textureManager().unload(texture);
      }
      m_dropped.clear();
      images[page]->setExternalTexture(renderer, pageTexture(renderer, cache[page]));
    });
  }

  void ViewerWindow::fillMarks(std::size_t i) {
    if (i >= m_markLayers.size() || m_markLayers[i] == nullptr) {
      return;
    }
    Node* layer = m_markLayers[i];
    while (!layer->children().empty()) {
      (void)layer->removeChild(layer->children().back().get());
    }
    const float zoom = m_model.zoom();
    const auto mark = [&](const RectF& r, ColorRole role, float opacity) {
      auto box = ui::box({.fill = colorSpecFromRole(role), .radius = 2.0F, .width = (r.x1 - r.x0) * zoom,
                          .height = (r.y1 - r.y0) * zoom, .opacity = opacity});
      box->setParticipatesInLayout(false);
      box->setPosition(r.x0 * zoom, r.y0 * zoom);
      layer->addChild(std::move(box));
    };
    const auto current = m_model.currentHit();
    const auto& hits = m_model.hits(i);
    for (std::size_t k = 0; k < hits.size(); ++k) {
      const bool isCurrent = current && current->page == i && current->index == k;
      mark(hits[k], isCurrent ? ColorRole::TermFindHitCurrent : ColorRole::TermFindHit, 1.0F);
    }
    if (m_selection && m_selection->page == i) {
      for (const RectF& r : m_selection->rects) {
        mark(r, ColorRole::Accent, 0.35F);
      }
    }
    for (const PendingInk& ink : m_pendingInk) {
      if (ink.page == i) {
        for (std::size_t k = 1; k < ink.stroke.size(); ++k) {
          addInkSegment(layer, ink.stroke[k - 1], ink.stroke[k]);
        }
      }
    }
    if (m_drawing && m_strokePage == i) {
      for (std::size_t k = 1; k < m_stroke.size(); ++k) {
        addInkSegment(layer, m_stroke[k - 1], m_stroke[k]);
      }
    }
    if (m_stamp && m_stamp->page == i) {
      const float s = m_window.uiScale();
      const RectF& b = m_stamp->box;
      auto frame = ui::box({.border = colorSpecFromRole(ColorRole::Accent), .borderWidth = 1.5F * s,
                            .width = (b.x1 - b.x0) * zoom, .height = (b.y1 - b.y0) * zoom});
      frame->setParticipatesInLayout(false);
      frame->setPosition(b.x0 * zoom, b.y0 * zoom);
      layer->addChild(std::move(frame));
      const float side = kStampHandle * s;
      auto handle = ui::box({.fill = colorSpecFromRole(ColorRole::Accent), .radius = side / 2.0F, .width = side,
                             .height = side});
      handle->setParticipatesInLayout(false);
      handle->setPosition(b.x1 * zoom - side / 2.0F, b.y1 * zoom - side / 2.0F);
      layer->addChild(std::move(handle));
    }
    if (m_notePoint && m_notePoint->first == i) {
      const float s = m_window.uiScale();
      const float side = 14.0F * s;
      auto pin = ui::box({.fill = fixedColorSpec(rgbHex(kNoteRgb)), .radius = 3.0F * s, .width = side, .height = side});
      pin->setParticipatesInLayout(false);
      pin->setPosition(m_notePoint->second.x * zoom - side / 2.0F, m_notePoint->second.y * zoom - side / 2.0F);
      layer->addChild(std::move(pin));
    }
  }

  void ViewerWindow::addInkSegment(Node* layer, const annot::Point& a, const annot::Point& b) {
    // Round dots half a pen width apart read as one line; the page render replaces them.
    const float zoom = m_model.zoom();
    const float pen = kInkWidth * zoom;
    const float length = std::hypot(b.x - a.x, b.y - a.y) * zoom;
    const int steps = std::max(1, static_cast<int>(std::ceil(length / (pen * 0.5F))));
    for (int k = 1; k <= steps; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(steps);
      auto dot = ui::box({.fill = fixedColorSpec(rgbHex(kInkRgb)), .radius = pen / 2.0F, .width = pen, .height = pen});
      dot->setParticipatesInLayout(false);
      dot->setPosition((a.x + (b.x - a.x) * t) * zoom - pen / 2.0F, (a.y + (b.y - a.y) * t) * zoom - pen / 2.0F);
      layer->addChild(std::move(dot));
    }
  }

  void ViewerWindow::refreshMarks() {
    for (std::size_t i = 0; i < m_markLayers.size(); ++i) {
      fillMarks(i);
    }
    refreshLabels();
    m_window.invalidate();
  }

  void ViewerWindow::refreshLabels() {
    if (m_mode != Mode::Pdf) {
      return;
    }
    const std::size_t page = currentPage();
    // Scrolling does not overwrite a page number the user is typing.
    if (m_pageInput != nullptr && m_window.input().focusedArea() != m_pageInput->inputArea()) {
      m_pageInput->setValue(std::to_string(page + 1));
    }
    refreshResults();
    if (page != m_shownPage) {
      const float s = m_window.uiScale();
      for (const std::size_t i : {m_shownPage, page}) {
        if (i < m_thumbFrames.size() && m_thumbFrames[i] != nullptr) {
          m_thumbFrames[i]->setBorder(colorSpecFromRole(i == page ? ColorRole::Accent : ColorRole::Separator),
                                      i == page ? 2.0F * s : 1.0F);
        }
      }
      m_shownPage = page;
    }
    if (const auto entry = currentEntry(); entry != m_shownEntry) {
      for (const auto i : {m_shownEntry, entry}) {
        if (i && *i < m_outlineRows.size() && m_outlineRows[*i] != nullptr) {
          m_outlineRows[*i]->setFill(i == entry ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                                                : clearColorSpec());
        }
      }
      m_shownEntry = entry;
    }
    m_window.invalidate();
  }

  TextureHandle ViewerWindow::pageTexture(Renderer& renderer, Page& page) {
    if (page.pixels != nullptr) {
      if (page.texture.valid()) {
        m_dropped.push_back(page.texture);
      }
      const auto bytes = page.pixels->bytes();
      page.texture = renderer.textureManager().loadFromRaw(bytes.data(), bytes.size(), static_cast<int>(page.width),
                                                           static_cast<int>(page.height), static_cast<int>(page.stride),
                                                           PixmapFormat::BGRA);
      page.pixels.reset();
    }
    if (page.texture.valid() && page.texture.generation != renderer.textureManager().generation()) {
      // The GL context was lost with the texture; render the page again.
      page = {};
      DeferredCall::callLaterIfAlive(m_alive, [this, generation = m_generation] {
        if (generation == m_generation) {
          requestVisiblePages();
        }
      });
    }
    return page.texture;
  }

  void ViewerWindow::requestRender(std::vector<Page>& cache, std::size_t page, std::uint32_t width) {
    cache[page].requestedWidth = width;
    takeJob();
    const std::uint64_t seq = ++m_renderSeq;
    m_helper->request(
        {.op = preview::op::Render, .a = static_cast<std::uint32_t>(page), .b = width, .c = kMaxPageHeight},
        [this, &cache, page, width, seq, generation = m_generation](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          endJob();
          // Sent last, so it runs after this reply is stored.
          DeferredCall::callLaterIfAlive(m_alive, [this, generation] {
            if (generation == m_generation) {
              pump();
            }
          });
          // A newer request for this page (a zoom, an edit) replaces this reply.
          if (page >= cache.size() || cache[page].requestedWidth != width) {
            return;
          }
          const auto& m = reply.msg;
          // Untrusted sizes: rows must fit the stride and the mapping.
          const bool valid = m.op == preview::op::Page && reply.data != nullptr && m.b > 0 && m.c > 0
              && m.b <= kMaxRenderWidth && m.c <= kMaxPageHeight && m.b <= m.d / 4
              && static_cast<std::uint64_t>(m.d) * m.c <= reply.data->bytes().size();
          if (!valid) {
            cache[page].requestedWidth = 0;
            return;
          }
          if (&cache == &m_pages && std::erase_if(m_pendingInk, [&](const PendingInk& ink) {
                return ink.page == page && ink.shownAfter && *ink.shownAfter < seq;
              }) > 0) {
            fillMarks(page);
          }
          Page& slot = cache[page];
          slot.pixels = reply.data;
          slot.width = m.b;
          slot.height = m.c;
          slot.stride = m.d;
          showRender(cache, page);
          if (page == 0 && &cache == &m_pages) {
            kLog.info("shown pdf page 1");
          }
        }
    );
  }

  void ViewerWindow::search(const std::string& query) {
    const bool wasOpen = !m_searchText.empty();
    m_searchText = capQuery(query);
    // The old query's rows go in place; the list refills as the scan runs.
    for (const ResultRow& r : m_resultRows) {
      (void)m_resultList->removeChild(r.row);
    }
    m_resultRows.clear();
    m_shownHit.reset();
    m_snippets.clear();
    if (wasOpen != !m_searchText.empty()) {
      if (wasOpen) {
        setSidebarView(m_sidebarView == SidebarView::Results ? m_viewBeforeSearch : m_sidebarView);
      } else {
        m_viewBeforeSearch = m_sidebarView;
        setSidebarView(SidebarView::Results);
      }
      // The switch gains or loses its Results choice.
      rebuild();
    }
    // Its own counter: a new search drops the old search's finds and snippets, not the edits, renders or saves in flight.
    ++m_searchGeneration;
    if (m_searchText.empty()) {
      m_model.clearSearch();
    } else {
      m_model.startSearch(m_searchText);
      // With a find of the old search in flight, its reply starts the new scan.
      scanNext();
    }
    refreshMarks();
  }

  void ViewerWindow::scanNext() {
    const auto page = m_model.nextPageToScan();
    if (!page || !m_helper || m_scanPending) {
      return;
    }
    m_scanPending = true;
    const std::string& q = m_searchText;
    m_helper->request(
        {.op = preview::op::Find, .a = static_cast<std::uint32_t>(*page)},
        std::vector<std::uint8_t>(q.begin(), q.end()),
        [this, page = *page, generation = m_generation, search = m_searchGeneration](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          m_scanPending = false;
          if (search != m_searchGeneration) {
            scanNext();
            return;
          }
          std::vector<RectF> hits;
          if (reply.msg.op == preview::op::FindReply && reply.msg.b > 0 && reply.data != nullptr
              && reply.data->bytes().size() >= static_cast<std::size_t>(reply.msg.b) * sizeof(RectF)) {
            hits = readRects(reply.data->bytes().data(), reply.msg.b);
          }
          const bool first = m_model.hitCount() == 0 && !hits.empty();
          m_model.setHits(page, std::move(hits));
          if (first && m_model.nextHit(currentPage())) {
            showCurrentHit();
          }
          if (m_model.searchDone()) {
            kLog.info("search \"{}\" {}", m_searchText, countText());
          }
          scanNext();
          refreshMarks();
        }
    );
  }

  void ViewerWindow::showCurrentHit() {
    const auto hit = m_model.currentHit();
    if (!hit || m_scroll == nullptr) {
      return;
    }
    const RectF box = m_model.pageBox(hit->page);
    const RectF r = m_model.hits(hit->page)[hit->index];
    const float y = box.y0 + r.y0 * m_model.zoom();
    const float top = m_scroll->scrollOffset();
    if (y < top || y > top + m_scroll->height() - 40.0F) {
      m_scrollOffset = std::max(0.0F, y - m_scroll->height() / 3.0F);
      m_scroll->setScrollOffset(m_scrollOffset);
      requestVisiblePages();
    }
    kLog.info("shown hit page {}", hit->page + 1);
  }

  void ViewerWindow::moveHit(bool forward) {
    if (forward ? m_model.nextHit(currentPage()) : m_model.previousHit(currentPage())) {
      showCurrentHit();
      refreshMarks();
    }
  }

  void ViewerWindow::beginSelect(std::size_t page, float x, float y) {
    m_selection = Selection{.page = page, .x0 = x, .y0 = y, .x1 = x, .y1 = y};
    m_selecting = true;
  }

  void ViewerWindow::updateSelect(float x, float y) {
    if (!m_selection) {
      return;
    }
    m_selection->x1 = x;
    m_selection->y1 = y;
    requestSelect();
  }

  void ViewerWindow::requestSelect() {
    // One select in flight; a newer drag position waits for it and goes next.
    if (m_selectPending) {
      m_selectDirty = true;
      return;
    }
    if (!m_selection || !m_helper) {
      return;
    }
    m_selectPending = true;
    m_selectDirty = false;
    const Selection& s = *m_selection;
    m_helper->request(
        {.op = preview::op::Select, .a = static_cast<std::uint32_t>(s.page), .b = s.unit},
        areaPayload(s.x0, s.y0, s.x1, s.y1),
        [this, generation = m_generation](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          m_selectPending = false;
          if (m_selection && reply.msg.op == preview::op::SelectReply) {
            const std::size_t count = reply.msg.b;
            const std::size_t length = reply.msg.a;
            const auto bytes = reply.data != nullptr ? reply.data->bytes() : std::span<const std::uint8_t>{};
            if (count * sizeof(RectF) + length <= bytes.size()) {
              m_selection->rects = readRects(bytes.data(), count);
              m_selection->text.assign(reinterpret_cast<const char*>(bytes.data()) + count * sizeof(RectF), length);
            } else {
              m_selection->rects.clear();
              m_selection->text.clear();
            }
            fillMarks(m_selection->page);
            m_window.invalidate();
          }
          if (m_selectDirty) {
            requestSelect();
          } else if (m_highlightOnSelect && !m_selecting && m_selection) {
            m_highlightOnSelect = false;
            if (!m_selection->rects.empty()) {
              addAnnotation(m_selection->page, preview::kAnnotHighlight, 0xffd400, annot::highlightBody(m_selection->rects));
            }
            m_selection.reset();
          }
        }
    );
  }

  void ViewerWindow::selectAll() {
    if (m_mode != Mode::Pdf || m_model.pageCount() == 0) {
      return;
    }
    const std::size_t page = currentPage();
    const RectF box = m_model.pageBox(page);
    const float zoom = m_model.zoom();
    m_selecting = false;
    m_selection = Selection{.page = page, .x1 = (box.x1 - box.x0) / zoom, .y1 = (box.y1 - box.y0) / zoom};
    requestSelect();
  }

  void ViewerWindow::copySelection() {
    TextClipboard* clipboard = Input::textClipboard();
    if (m_selection && !m_selection->text.empty() && clipboard != nullptr) {
      clipboard->setClipboardText(m_selection->text);
      kLog.info("copied {} bytes", m_selection->text.size());
    }
  }

  void ViewerWindow::copyPath() {
    TextClipboard* clipboard = Input::textClipboard();
    if (!m_path.empty() && clipboard != nullptr) {
      clipboard->setClipboardText(m_path.string());
    }
  }

  void ViewerWindow::showShareMenu(LinkShare* link) {
    if (m_path.empty()) {
      return;
    }
    std::vector<std::string> ids;
    ContextMenuControlEntry share = share::menuEntry(link, ids);
    ids.push_back("file.copy-path");
    share.children.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = i18n::tr("viewer.commands.copy-path")});
    float x = 0.0F;
    float y = 0.0F;
    if (m_shareButton != nullptr) {
      Node::mapToScene(m_shareButton, 0.0F, m_shareButton->height(), x, y);
    }
    m_window.openContextMenu(std::move(share.children), x, y, [window = &m_window, ids, link, path = m_path](std::int32_t id) {
      if (id <= 0 || static_cast<std::size_t>(id) > ids.size()) {
        return;
      }
      const std::string& command = ids[static_cast<std::size_t>(id) - 1];
      kLog.info("share {}", command);
      if (command.starts_with("share.device:") && link != nullptr) {
        link->send(command.substr(std::string_view("share.device:").size()), {path},
                   [](const std::string&, const std::string& error) {
                     kLog.info("share result {}", error.empty() ? "sent" : error);
                   });
      } else if (command == "share.email") {
        if (const std::string error = share::sendByEmail({path}); !error.empty()) {
          kLog.warn("{}", error);
        }
      } else {
        window->commands().run(command);
      }
    });
  }

  void ViewerWindow::searchFor(const std::string& query) {
    if (m_searchInput != nullptr) {
      m_searchInput->setValue(query);
    }
    search(query);
  }

  void ViewerWindow::focusSearch() {
    m_window.setSidebarVisible(true);
    if (m_searchInput != nullptr) {
      m_window.input().setFocus(m_searchInput->inputArea());
    }
  }

  void ViewerWindow::zoomIn() { zoomTo([this] { m_model.zoomBy(1.25F); }); }

  void ViewerWindow::zoomOut() { zoomTo([this] { m_model.zoomBy(0.8F); }); }

  void ViewerWindow::zoomFit() { zoomTo([this] { m_model.fitWidth(); }); }

  void ViewerWindow::zoomActual() { zoomTo([this] { m_model.setZoom(1.0F); }); }

  // The current page fills the view, its top at the top.
  void ViewerWindow::zoomFitPage() {
    if (m_mode != Mode::Pdf || m_scroll == nullptr) {
      return;
    }
    const std::size_t page = currentPage();
    const float before = m_model.zoom();
    zoomTo([this] {
      m_model.setViewportHeight(m_scroll->height() - 2.0F * kPagePadding * m_window.uiScale());
      m_model.fitPage();
    });
    if (m_model.zoom() == before) {
      goToPage(page);
    } else {
      m_scrollOffset = m_model.pageBox(page).y0;
    }
  }

  float ViewerWindow::widestPage() const {
    float w = 0.0F;
    for (std::size_t i = 0; i < m_model.pageCount(); ++i) {
      const RectF box = m_model.pageBox(i);
      w = std::max(w, box.x1 - box.x0);
    }
    return w;
  }

  void ViewerWindow::zoomTo(const std::function<void()>& apply, float anchorX, float anchorY, bool animate) {
    // The pages re-render at the new zoom; the column springs from the old size to the new one around the anchor,
    // and both scrolls keep the anchor in place.
    const float before = m_model.zoom();
    const float w0 = widestPage();
    apply();
    saveViewState(); // also a fit change at the same zoom, which returns below
    const float after = m_model.zoom();
    if (after <= 0.0F || before == after) {
      return;
    }
    const float f = after / before;
    if (m_scroll != nullptr && m_hscroll != nullptr) {
      const float pad = kPagePadding * m_window.uiScale();
      const float view = m_hscroll->width() - 2.0F * pad;
      const float ax = (anchorX < 0.0F ? m_scroll->width() / 2.0F : anchorX) - pad;
      const float ay = (anchorY < 0.0F ? m_scroll->height() / 2.0F : anchorY) - pad;
      m_scrollOffset = std::max(0.0F, (m_scroll->scrollOffset() + ay) * f - ay);
      // A page narrower than the view sits centred in it: measure the anchor from the page edge.
      const float w1 = w0 * f;
      const float edge0 = std::max(0.0F, (view - w0) / 2.0F);
      const float edge1 = std::max(0.0F, (view - w1) / 2.0F);
      const float u = m_hscroll->scrollOffset() + ax - edge0;
      m_hOffset = std::clamp(edge1 + u * f - ax, 0.0F, std::max(0.0F, w1 - view));
      m_zoomAnchorX = m_hOffset + ax;
      m_zoomAnchorY = m_scrollOffset + ay;
    }
    m_zoomFrom = animate ? 1.0F / f : 1.0F;
    if (m_zoomLabel != nullptr) {
      m_zoomLabel->setText(std::format("{}%", std::lround(after * 100.0F)));
    }
    rebuildContent();
  }

  void ViewerWindow::pinch(const InputArea::PinchData& data) {
    if (m_scroll == nullptr || m_hscroll == nullptr || m_column == nullptr) {
      return;
    }
    using Phase = InputArea::PinchData::Phase;
    const float pad = kPagePadding * m_window.uiScale();
    if (data.phase == Phase::Begin) {
      m_pinchStart = m_model.zoom();
      m_pinchScale = 1.0F;
      m_pinchX = data.localX;
      m_pinchY = data.localY;
      // Column coordinates of the point under the fingers.
      m_zoomAnchorX = m_hscroll->scrollOffset() + m_pinchX - pad;
      m_zoomAnchorY = m_scroll->scrollOffset() + m_pinchY - pad;
      return;
    }
    if (data.phase == Phase::End) {
      const float scale = m_pinchScale;
      m_pinchScale = 1.0F;
      if (data.cancelled || scale == 1.0F) {
        m_column->setScale(1.0F);
        m_window.invalidate();
        return;
      }
      // The column already shows the new size: commit it without a spring.
      zoomTo([this, scale] { m_model.zoomBy(scale); }, m_pinchX, m_pinchY, false);
      return;
    }
    m_pinchScale = std::clamp(static_cast<float>(data.scale), DocumentModel::kMinZoom / m_pinchStart,
                              DocumentModel::kMaxZoom / m_pinchStart);
    // Scaling the laid-out column follows the fingers every frame; the pages re-render once, at the end.
    m_column->setTransformOrigin(m_zoomAnchorX, m_zoomAnchorY);
    m_column->setScale(m_pinchScale);
    m_window.invalidate();
  }

  bool ViewerWindow::wheelZoom(const InputArea::PointerData& data) {
    if ((m_loop.wayland().keyboardModifiers() & KeyMod::Ctrl) == 0 || data.axis != PointerAxis::Vertical) {
      return false;
    }
    // One wheel notch (value 15) zooms by 10 %; a touchpad scales in proportion.
    const float factor = std::pow(1.1F, -static_cast<float>(data.axisValue) / 15.0F);
    zoomTo([this, factor] { m_model.zoomBy(factor); }, data.localX, data.localY);
    return true;
  }

  void ViewerWindow::toggleSidebar() { m_window.toggleSidebar(); }

  bool ViewerWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed) {
      return false;
    }
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    switch (event.sym) {
    case XKB_KEY_Left:
    case XKB_KEY_Page_Up:
      step(-1);
      return true;
    case XKB_KEY_Right:
    case XKB_KEY_Page_Down:
      step(1);
      return true;
    case XKB_KEY_Home:
    case XKB_KEY_End:
      if (m_mode == Mode::Pdf && m_scroll != nullptr && m_model.pageCount() > 0) {
        m_scroll->setScrollOffset(event.sym == XKB_KEY_Home ? 0.0F : m_model.pageBox(m_model.pageCount() - 1).y0);
        onScroll();
        return true;
      }
      return false;
    case XKB_KEY_F3:
    case XKB_KEY_Return:
      if (!m_searchText.empty()) {
        moveHit(!shift);
        return true;
      }
      return false;
    case XKB_KEY_Escape:
      if (m_stamp) {
        const std::size_t page = m_stamp->page;
        m_stamp.reset();
        fillMarks(page);
        m_window.invalidate();
        return true;
      }
      if (m_signing || m_notePoint) {
        m_signing = false;
        m_pendingSign.reset();
        m_notePoint.reset();
        rebuild();
        return true;
      }
      if (!m_searchText.empty()) {
        if (m_searchInput != nullptr) {
          m_searchInput->setValue("");
        }
        search("");
        return true;
      }
      m_selection.reset();
      rebuild();
      return true;
    default:
      return false;
    }
  }

  void ViewerWindow::forgetNodes() {
    m_scroll = nullptr;
    m_hscroll = nullptr;
    m_column = nullptr;
    m_noteInput = nullptr;
    for (auto& [key, tile] : m_tiles) {
      tile.node = nullptr;
    }
    m_tileLayers.assign(m_pages.size(), nullptr);
    m_pageImages.assign(m_pages.size(), nullptr);
    m_markLayers.assign(m_pages.size(), nullptr);
  }

  void ViewerWindow::forgetCaption() {
    m_pageInput = nullptr;
    m_zoomLabel = nullptr;
    m_shareButton = nullptr;
  }

  void ViewerWindow::forgetSidebar() {
    m_searchInput = nullptr;
    m_thumbScroll = nullptr;
    m_outlineRows.clear();
    m_thumbImages.assign(m_thumbs.size(), nullptr);
    m_thumbFrames.assign(m_thumbs.size(), nullptr);
    m_resultRows.clear();
    m_resultList = nullptr;
    m_resultCount = nullptr;
    m_shownHit.reset();
  }

  std::unique_ptr<Node> ViewerWindow::build(Renderer& renderer, float width, float height) {
    const float s = m_window.uiScale();
    for (TextureHandle& texture : m_dropped) {
      renderer.textureManager().unload(texture);
    }
    m_dropped.clear();
    forgetNodes();
    m_areaWidth = width;
    m_areaHeight = height;
    // Fit width uses the page area inside the padding; set it before the toolbar shows the zoom.
    m_model.setViewportWidth(std::max(100.0F, pageAreaWidth() - 2.0F * kPagePadding * s));
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    switch (m_mode) {
    case Mode::Empty:
      root->addChild(buildMessage(i18n::tr("viewer.empty.title"), i18n::tr("viewer.empty.caption")));
      break;
    case Mode::Loading:
      root->addChild(ui::node({.flexGrow = 1.0F}));
      break;
    case Mode::Error:
      root->addChild(buildMessage(m_path.filename().string(), m_error));
      break;
    case Mode::Image: {
      auto image = ui::image({.fit = ImageFit::Contain, .padding = Style::spaceLg * s, .flexGrow = 1.0F});
      image->setSourceRaw(renderer, m_image.bytes.data(), m_image.bytes.size(), static_cast<int>(m_image.width),
                          static_cast<int>(m_image.height), static_cast<int>(m_image.stride), PixmapFormat::RGBA);
      root->addChild(std::move(image));
      break;
    }
    case Mode::Pdf: {
      if (m_signing) {
        root->addChild(buildSignaturePad());
        break;
      }
      root->addChild(buildPages(renderer));
      break;
    }
    }
    if (m_mode == Mode::Pdf) {
      DeferredCall::callLaterIfAlive(m_alive, [this, generation = m_generation] {
        if (generation == m_generation) {
          requestVisiblePages();
        }
      });
    }
    return root;
  }

  std::unique_ptr<Node> ViewerWindow::buildCaption() {
    forgetCaption();
    const float s = m_window.uiScale();
    auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    const auto footnote = [s](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeCaption * s,
                        .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1});
    };
    if (m_mode == Mode::Image && m_folder.size() > 1) {
      bar->addChild(footnote(std::format("{} of {}", m_folderIndex + 1, m_folder.size())));
    }
    const auto addShare = [&] {
      bar->addChild(ui::button({.out = &m_shareButton, .glyph = std::string("share"), .glyphSize = Tokens::sizeSymbol * s,
                                .variant = ButtonVariant::Ghost, .tooltip = i18n::tr("desk.share.title"),
                                .radius = Style::scaledRadius(Tokens::radiusR8, s), .width = Tokens::sizeControlM * s,
                                .height = Tokens::sizeControlM * s,
                                .onClick = [this] { m_window.commands().run("file.share"); }}));
    };
    if (m_mode != Mode::Pdf) {
      if (m_mode == Mode::Image) {
        addShare();
      }
      return bar;
    }
    auto page = ui::input({
        .value = std::to_string(currentPage() + 1),
        .fontSize = Style::fontSizeCaption * s,
        .textAlign = TextAlign::Center,
        .width = 56.0F * s,
        .onSubmit =
            [this](const std::string& text) {
              std::size_t number = 0;
              const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
              if (ec == std::errc{} && end == text.data() + text.size() && number > 0) {
                goToPage(number - 1);
              }
              m_pageInput->setValue(std::to_string(currentPage() + 1));
            },
        .onFocusLoss = [this] { m_pageInput->setValue(std::to_string(currentPage() + 1)); },
    });
    page->inputArea()->setTabFocusKey("page");
    m_pageInput = page.get();
    bar->addChild(std::move(page));
    bar->addChild(footnote(std::format("/ {}", m_model.pageCount())));
    const float small = Tokens::sizeControlS * s;
    auto zoom = ui::row({.align = FlexAlign::Center, .fill = colorSpecFromRole(ColorRole::Fill1),
                         .radius = Style::scaledRadius(Tokens::radiusR8, s)});
    const auto zoomButton = [&](const char* glyph, const std::string& tip, void (ViewerWindow::*run)()) {
      zoom->addChild(ui::button({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbolInline * s,
                                 .variant = ButtonVariant::Ghost, .tooltip = std::string(tip),
                                 .radius = Style::scaledRadius(Tokens::radiusR8, s), .width = small, .height = small,
                                 .onClick = [this, run] { (this->*run)(); }}));
    };
    zoomButton("minus", i18n::tr("viewer.commands.zoom-out"), &ViewerWindow::zoomOut);
    zoom->addChild(ui::button({.out = &m_zoomLabel, .text = std::format("{}%", std::lround(m_model.zoom() * 100.0F)),
                               .variant = ButtonVariant::Ghost, .tooltip = i18n::tr("viewer.commands.zoom-fit"),
                               .radius = Style::scaledRadius(Tokens::radiusR8, s), .height = small,
                               .onClick = [this] { zoomFit(); }}));
    zoomButton("plus", i18n::tr("viewer.commands.zoom-in"), &ViewerWindow::zoomIn);
    bar->addChild(std::move(zoom));
    bar->addChild(ui::row({.flexGrow = 1.0F}));
    static constexpr std::array<std::pair<const char*, const char*>, 5> kTools{
        {{"cursor-text", "viewer.tools.select-text"}, {"highlight", "viewer.tools.highlight"}, {"note", "viewer.tools.note"}, {"pencil", "viewer.tools.draw"}, {"signature", "viewer.tools.sign"}}};
    // A split pane has room for one tool button: it shows the current tool and opens the others as a menu.
    if (m_window.contentCards()) {
      Button* tool = nullptr;
      bar->addChild(ui::button({.out = &tool, .glyph = std::string(kTools[static_cast<std::size_t>(m_tool)].first),
                                .glyphSize = Tokens::sizeSymbolInline * s, .variant = ButtonVariant::Ghost,
                                .tooltip = i18n::tr(kTools[static_cast<std::size_t>(m_tool)].second),
                                .radius = Style::scaledRadius(Tokens::radiusR8, s), .width = small, .height = small}));
      tool->setOnClick([this, tool] {
        std::vector<ContextMenuControlEntry> entries;
        for (std::size_t i = 0; i < kTools.size(); ++i) {
          entries.push_back({.id = static_cast<std::int32_t>(i), .label = i18n::tr(kTools[i].second), .radio = true,
                             .toggleState = i == static_cast<std::size_t>(m_tool) ? 1 : 0});
        }
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(tool, x, y);
        m_window.openContextMenu(std::move(entries), x, y + tool->height(),
                                 [this](std::int32_t id) { setTool(static_cast<Tool>(id)); });
      });
    } else {
      std::vector<ui::SegmentedOption> options;
      for (const auto& [glyph, tip] : kTools) {
        options.push_back({.glyph = glyph, .tooltip = i18n::tr(tip)});
      }
      bar->addChild(ui::segmented({.options = std::move(options),
                                   .selectedIndex = static_cast<std::size_t>(m_tool),
                                   .compact = true,
                                   .onChange = [this](std::size_t i) { setTool(static_cast<Tool>(i)); }}));
    }
    addShare();
    if (!m_window.sidebarVisible()) {
      bar->addChild(ui::button({.glyph = std::string("search"), .glyphSize = Tokens::sizeSymbol * s,
                                .variant = ButtonVariant::Ghost, .tooltip = i18n::tr("viewer.commands.find"),
                                .radius = Style::scaledRadius(Tokens::radiusR8, s), .width = Tokens::sizeControlM * s,
                                .height = Tokens::sizeControlM * s, .onClick = [this] { focusSearch(); }}));
    }
    return bar;
  }

  // The note field floats beside its pin on the page, so the caption tools never move.
  std::unique_ptr<Node> ViewerWindow::buildNoteField(std::size_t page, annot::Point at, float pageWidth) {
    const float s = m_window.uiScale();
    const float pad = Tokens::spaceP8 * s;
    const float w = std::min(240.0F * s, pageWidth);
    auto card = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s, .padding = pad,
                            .fill = colorSpecFromRole(ColorRole::Popover),
                            .radius = Style::scaledRadius(Tokens::radiusR12, s), .width = w});
    card->setBorder(colorSpecFromRole(ColorRole::Stroke), 0.5F * s);
    auto note = ui::input({
        .placeholder = i18n::tr("viewer.tools.note"),
        .width = w - 2.0F * pad,
        .onSubmit =
            [this, page, at](const std::string& text) {
              if (!text.empty()) {
                addAnnotation(page, preview::kAnnotNote, kNoteRgb, annot::noteBody(at, text));
              }
              m_notePoint.reset();
              rebuild();
            },
    });
    m_noteInput = note.get();
    card->addChild(std::move(note));
    card->addChild(ui::label({.text = i18n::tr("viewer.note.hint"), .fontSize = Tokens::typeFootnote.size * s,
                              .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
    card->setParticipatesInLayout(false);
    const float zoom = m_model.zoom();
    float x = at.x * zoom - w - pad;
    if (x < 0.0F) {
      x = at.x * zoom + 2.0F * pad;
    }
    card->setPosition(std::clamp(x, 0.0F, std::max(0.0F, pageWidth - w)), std::max(0.0F, at.y * zoom - pad));
    return card;
  }

  float ViewerWindow::pageAreaWidth() const { return m_areaWidth; }

  std::unique_ptr<Node> ViewerWindow::buildPages(Renderer& renderer) {
    const float s = m_window.uiScale();
    const float pad = kPagePadding * s;
    auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = pad});
    scroll->setFlexGrow(1.0F);
    const float area = pageAreaWidth();
    // A page wider than the view pans sideways in its own scroll view; vertical scrolling passes through it.
    auto hscroll = ui::scrollView({.viewportPaddingH = pad, .viewportPaddingV = 0.0F});
    hscroll->setOrientation(ScrollOrientation::Horizontal);
    hscroll->setScrollbarVisible(false);
    hscroll->setSize(area, m_model.contentHeight());
    auto columnNode = ui::column({.align = FlexAlign::Center, .gap = DocumentModel::kPageGap,
                                  .width = std::max(area - 2.0F * pad, widestPage())});
    Node* column = columnNode.get();
    const float zoom = m_model.zoom();
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = box.x1 - box.x0;
      const float h = box.y1 - box.y0;
      auto sheet = ui::inputArea({.cursorShape = ::CursorShape::Text, .width = w, .height = h});
      sheet->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE}));
      InputArea* sheetArea = sheet.get();
      sheet->setClipChildren(true);
      auto paper = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .width = w, .height = h});
      paper->setParticipatesInLayout(false);
      sheet->addChild(std::move(paper));
      auto image = ui::image({.fit = ImageFit::Contain, .width = w, .height = h});
      if (const TextureHandle texture = pageTexture(renderer, m_pages[i]); texture.valid()) {
        image->setExternalTexture(renderer, texture);
      }
      image->setParticipatesInLayout(false);
      m_pageImages[i] = image.get();
      sheet->addChild(std::move(image));
      auto tiles = ui::box({.width = w, .height = h});
      tiles->setParticipatesInLayout(false);
      tiles->setClipChildren(true);
      m_tileLayers[i] = tiles.get();
      sheet->addChild(std::move(tiles));
      auto layer = ui::box({.width = w, .height = h});
      layer->setParticipatesInLayout(false);
      m_markLayers[i] = layer.get();
      sheet->addChild(std::move(layer));
      if (m_notePoint && m_notePoint->first == i) {
        sheet->addChild(buildNoteField(i, m_notePoint->second, w));
      }
      sheet->setOnPress([this, i, zoom, sheetArea](const InputArea::PointerData& d) {
        if (d.button == BTN_LEFT) {
          onPagePress(i, d.localX / zoom, d.localY / zoom, d.pressed, d.time);
        } else if (d.button == BTN_MIDDLE && m_scroll != nullptr && m_hscroll != nullptr) {
          m_pan = d.pressed ? std::optional<Pan>(Pan{.x = d.sceneX, .y = d.sceneY, .top = m_scroll->scrollOffset(),
                                                     .left = m_hscroll->scrollOffset()})
                            : std::nullopt;
          sheetArea->setCursorShape(d.pressed ? ::CursorShape::Grabbing
                                                : ::CursorShape::Text);
        }
      });
      sheet->setOnMotion([this, i, zoom, sheetArea](const InputArea::PointerData& d) {
        if (m_pan && m_scroll != nullptr && m_hscroll != nullptr) {
          m_hscroll->setScrollOffset(m_pan->left - (d.sceneX - m_pan->x));
          m_scroll->setScrollOffset(m_pan->top - (d.sceneY - m_pan->y));
          return;
        }
        const bool onLink = m_tool == Tool::Select && !m_selecting && linkAt(i, d.localX / zoom, d.localY / zoom) != nullptr;
        sheetArea->setCursorShape(onLink ? ::CursorShape::Pointer : ::CursorShape::Text);
        onPageMotion(d.localX / zoom, d.localY / zoom);
      });
      column->addChild(std::move(sheet));
    }
    hscroll->content()->addChild(std::move(columnNode));
    for (std::size_t i = 0; i < m_markLayers.size(); ++i) {
      fillMarks(i);
    }
    // Tiles of the old zoom go; the stretched whole page shows until the new ones come.
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
      Tile& tile = it->second;
      const RectF box = m_model.pageBox(tile.page);
      if (tile.pageWidth != static_cast<std::uint32_t>(std::lround((box.x1 - box.x0) * s))) {
        dropTile(tile);
        it = m_tiles.erase(it);
        continue;
      }
      if (const TextureHandle texture = pageTexture(renderer, tile.px); texture.valid()) {
        tile.node = placeTile(tile);
        tile.node->setExternalTexture(renderer, texture);
      }
      ++it;
    }
    hscroll->setOnScrollChanged([this](float offset) { m_hOffset = offset; });
    hscroll->requestScrollToOffset(m_hOffset);
    m_hscroll = hscroll.get();
    m_column = column;
    scroll->content()->addChild(std::move(hscroll));
    scroll->setOnScrollChanged([this](float) {
      onScroll();
      m_window.invalidate();
    });
    scroll->setOnPinch([this](const InputArea::PinchData& data) { pinch(data); });
    scroll->setAxisFilter([this](const InputArea::PointerData& data) { return wheelZoom(data); });
    m_scroll = scroll.get();
    m_restoreScroll = m_scrollOffset > 0.0F;
    if (m_pinchScale != 1.0F) {
      // A render reply rebuilt the view mid-pinch: keep the live scale.
      column->setTransformOrigin(m_zoomAnchorX, m_zoomAnchorY);
      column->setScale(m_pinchScale);
    }
    if (m_zoomFrom != 1.0F) {
      column->setTransformOrigin(m_zoomAnchorX, m_zoomAnchorY);
      column->setScale(m_zoomFrom);
      m_zoomAnim = m_window.animations().drive(
          m_zoomAnim, m_zoomFrom, 1.0F, Style::spring(Tokens::springMove),
          [this, column](float v) {
            if (m_column == column) {
              column->setScale(v);
              m_window.invalidate();
            }
          },
          [this] { m_zoomAnim = 0; }, nullptr);
      m_zoomFrom = 1.0F;
    }
    return scroll;
  }

  std::unique_ptr<Node> ViewerWindow::buildSearch() {
    forgetSidebar();
    if (m_mode != Mode::Pdf) {
      return nullptr;
    }
    const float s = m_window.uiScale();
    std::vector<std::unique_ptr<Node>> arrows;
    if (!m_searchText.empty()) {
      for (const bool forward : {false, true}) {
        arrows.push_back(ui::button({.glyph = std::string(forward ? "chevron-down" : "chevron-up"),
                                     .glyphSize = Tokens::sizeSymbolInline * s, .variant = ButtonVariant::Ghost,
                                     .tooltip = i18n::tr(forward ? "viewer.search.next" : "viewer.search.previous"),
                                     .radius = Tokens::sizeControlS * s / 2.0F, .width = Tokens::sizeControlS * s,
                                     .height = Tokens::sizeControlS * s, .onClick = [this, forward] { moveHit(forward); }}));
      }
    }
    auto capsule = ui::searchCapsule({
        .out = &m_searchInput,
        .placeholder = i18n::tr("viewer.search.placeholder"),
        .height = Tokens::sizeControlL,
        .scale = m_window.uiScale(),
        .value = m_searchText,
        .onChange = [this](const std::string& text) { search(text); },
        .onSubmit = [this](const std::string&) { moveHit((m_loop.wayland().keyboardModifiers() & KeyMod::Shift) == 0); },
        .onKeyEvent = [this](std::uint32_t sym, std::uint32_t) {
          if (sym != XKB_KEY_Escape || m_searchText.empty()) {
            return false;
          }
          m_searchInput->setValue("");
          search("");
          return true;
        },
        .trailing = std::move(arrows),
    });
    // A rebuild hands the keyboard focus to the new field by this key.
    m_searchInput->inputArea()->setTabFocusKey("search");
    return capsule;
  }

  void ViewerWindow::setSidebarView(SidebarView view) {
    if (view != m_sidebarView) {
      m_sidebarView = view;
      rebuild();
    }
  }

  std::unique_ptr<Node> ViewerWindow::buildSidebarList(Renderer& renderer, float width) {
    if (m_mode != Mode::Pdf) {
      return nullptr;
    }
    const float s = m_window.uiScale();
    if (m_sidebarView == SidebarView::Outline && m_outline.empty()) {
      m_sidebarView = SidebarView::Pages;
    }
    std::vector<SidebarView> views{SidebarView::Pages};
    if (!m_outline.empty()) {
      views.push_back(SidebarView::Outline);
    }
    if (!m_searchText.empty()) {
      views.push_back(SidebarView::Results);
    }
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .flexGrow = 1.0F});
    if (views.size() > 1) {
      std::vector<ui::SegmentedOption> options;
      for (const SidebarView v : views) {
        options.push_back({.label = i18n::tr(v == SidebarView::Pages ? "viewer.sidebar.pages" : v == SidebarView::Outline ? "viewer.sidebar.outline" : "viewer.sidebar.results")});
      }
      column->addChild(ui::segmented({.options = std::move(options),
                                      .selectedIndex = static_cast<std::size_t>(std::ranges::find(views, m_sidebarView) - views.begin()),
                                      .compact = true,
                                      .equalSegmentWidths = true,
                                      .width = width,
                                      .onChange = [this, views](std::size_t i) { setSidebarView(views.at(i)); }}));
    }
    auto list = m_sidebarView == SidebarView::Outline ? buildOutline(width)
                : m_sidebarView == SidebarView::Results ? buildResults(width)
                                                         : buildThumbs(renderer, width);
    list->setFlexGrow(1.0F);
    column->addChild(std::move(list));
    return column;
  }

  std::unique_ptr<Node> ViewerWindow::buildResults(float width) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F});
    scroll->content()->setGap(Tokens::spaceP4 * s);
    scroll->content()->setAlign(FlexAlign::Stretch);
    auto count = ui::label({.out = &m_resultCount, .text = countText(), .fontSize = Tokens::typeFootnote.size * s,
                            .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2),
                            .maxLines = 1});
    auto head = ui::row({.align = FlexAlign::Center});
    head->setPadding(0.0F, Tokens::spaceP8 * s);
    head->addChild(std::move(count));
    scroll->content()->addChild(std::move(head));
    m_resultList = scroll->content();
    m_resultWidth = width;
    refreshResults();
    return scroll;
  }

  std::string ViewerWindow::countText() const {
    if (m_searchText.empty() || (m_model.hitCount() == 0 && !m_model.searchDone())) {
      return {};
    }
    if (m_model.hitCount() == 0) {
      return i18n::tr("viewer.search.no-results");
    }
    return i18n::tr(m_model.searchDone() ? "viewer.search.count" : "viewer.search.count-partial", "current", m_model.currentOrdinal(),
                    "total", m_model.hitCount());
  }

  // Rows follow the scan (pages in order) in place; the current hit is a raised lens row.
  void ViewerWindow::refreshResults() {
    if (m_resultCount != nullptr) {
      (void)m_resultCount->setText(countText());
    }
    if (m_resultList == nullptr || m_searchText.empty()) {
      return;
    }
    const float s = m_window.uiScale();
    std::size_t page = m_resultRows.empty() ? 0 : m_resultRows.back().page;
    std::size_t index = m_resultRows.empty() ? 0 : m_resultRows.back().index + 1;
    for (; page < m_model.pageCount() && m_resultRows.size() < kMaxResultRows; ++page, index = 0) {
      const std::size_t hits = m_model.hits(page).size();
      for (; index < hits && m_resultRows.size() < kMaxResultRows; ++index) {
        auto row = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F,
                               .radius = Style::scaledRadius(Tokens::componentSidebarRowRadius, s), .width = m_resultWidth,
                               .onClick = [this, page, index] {
                                 if (page < m_model.pageCount() && index < m_model.hits(page).size()) {
                                   m_model.setCurrentHit({.page = page, .index = index});
                                   showCurrentHit();
                                   refreshMarks();
                                 }
                               }});
        row->setPadding(Tokens::spaceP8 * s, Tokens::spaceP8 * s);
        const auto snippet = m_snippets.find(page << 32 | index);
        Label* text = nullptr;
        row->addChild(ui::label({.out = &text, .text = " ", .fontSize = Style::fontSizeCaption * s,
                                 .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2,
                                 .ellipsize = TextEllipsize::End}));
        text->setUseMarkup(true);
        if (snippet != m_snippets.end()) {
          (void)text->setText(hitMarkup(snippet->second, m_searchText, isCurrentRow(page, index)));
        }
        row->addChild(ui::label({.text = i18n::tr("viewer.sidebar.page", "number", page + 1), .fontSize = Tokens::typeFootnote.size * s,
                                 .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2),
                                 .maxLines = 1}));
        m_resultRows.push_back({.page = page, .index = index, .row = row.get(), .snippet = text});
        m_resultList->addChild(std::move(row));
      }
    }
    const auto hit = m_model.currentHit();
    const auto now = hit ? std::optional<std::pair<std::size_t, std::size_t>>({hit->page, hit->index}) : std::nullopt;
    if (now != m_shownHit) {
      for (ResultRow& r : m_resultRows) {
        const bool on = now && r.page == now->first && r.index == now->second;
        r.row->setFill(on ? colorSpecFromRole(ColorRole::Lens) : clearColorSpec());
        r.row->setBorder(on ? colorSpecFromRole(ColorRole::LensEdge) : clearColorSpec(), on ? 0.5F * s : 0.0F);
        const auto snippet = m_snippets.find(static_cast<std::uint64_t>(r.page) << 32 | r.index);
        if (on != isCurrentRow(r.page, r.index) && snippet != m_snippets.end() && r.snippet != nullptr) {
          (void)r.snippet->setText(hitMarkup(snippet->second, m_searchText, on));
        }
      }
      m_shownHit = now;
    }
    requestSnippet();
  }

  bool ViewerWindow::isCurrentRow(std::size_t page, std::size_t index) const {
    return m_shownHit && m_shownHit->first == page && m_shownHit->second == index;
  }

  void ViewerWindow::requestSnippet() {
    if (m_snippetPending || !m_helper) {
      return;
    }
    for (const ResultRow& r : m_resultRows) {
      const std::uint64_t key = static_cast<std::uint64_t>(r.page) << 32 | r.index;
      if (m_snippets.contains(key) || r.index >= m_model.hits(r.page).size()) {
        continue;
      }
      const RectF hit = m_model.hits(r.page)[r.index];
      const float mid = (hit.y0 + hit.y1) / 2.0F;
      m_snippetPending = true;
      m_helper->request(
          {.op = preview::op::Select, .a = static_cast<std::uint32_t>(r.page), .b = preview::kSelectLine},
          areaPayload(hit.x0, mid, hit.x1, mid),
          [this, key, generation = m_generation, search = m_searchGeneration](const preview::HelperReply& reply) {
            if (generation != m_generation) {
              return;
            }
            m_snippetPending = false;
            if (search != m_searchGeneration) {
              requestSnippet();
              return;
            }
            std::string text;
            if (reply.msg.op == preview::op::SelectReply && reply.data != nullptr) {
              const auto bytes = reply.data->bytes();
              const std::size_t at = static_cast<std::size_t>(reply.msg.b) * sizeof(RectF);
              if (at + reply.msg.a <= bytes.size()) {
                text = snippetAround(std::string(reinterpret_cast<const char*>(bytes.data()) + at, reply.msg.a), m_searchText);
              }
            }
            for (const ResultRow& row : m_resultRows) {
              if ((static_cast<std::uint64_t>(row.page) << 32 | row.index) == key && row.snippet != nullptr) {
                (void)row.snippet->setText(hitMarkup(text, m_searchText, isCurrentRow(row.page, row.index)));
              }
            }
            m_snippets[key] = std::move(text);
            m_window.invalidate();
            requestSnippet();
          });
      return;
    }
  }

  std::optional<std::size_t> ViewerWindow::currentEntry() const {
    const std::size_t page = currentPage();
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < m_outline.size(); ++i) {
      const auto& target = m_outline[i].page;
      if (target && *target <= page && (!best || *target >= *m_outline[*best].page)) {
        best = i;
      }
    }
    return best;
  }

  std::unique_ptr<Node> ViewerWindow::buildOutline(float width) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F});
    const float height = Tokens::componentSidebarRow * s;
    const auto current = currentEntry();
    m_shownEntry = current;
    m_outlineRows.assign(m_outline.size(), nullptr);
    for (std::size_t i = 0; i < m_outline.size(); ++i) {
      const OutlineEntry& entry = m_outline[i];
      auto area = ui::inputArea({.cursorShape = ::CursorShape::Pointer, .width = width, .height = height,
                                 .onClick = [this, i](const InputArea::PointerData&) {
                                   if (i < m_outline.size() && m_outline[i].page) {
                                     goToPage(*m_outline[i].page);
                                   }
                                 }});
      auto row = ui::row({.align = FlexAlign::Center,
                          .fill = i == current ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                                               : clearColorSpec(),
                          .radius = Style::scaledRadius(Tokens::componentSidebarRowRadius, s),
                          .width = width,
                          .height = height});
      // Deeper levels indent up to four steps; past that the title would have no room in the narrow sidebar.
      const float indent = static_cast<float>(std::min<std::uint32_t>(entry.depth, 4)) * Tokens::spaceP12 * s;
      row->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s + indent);
      row->addChild(ui::label({.text = entry.title, .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(entry.page ? ColorRole::Label1 : ColorRole::Label2),
                               .maxLines = 1, .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
      m_outlineRows[i] = row.get();
      area->addChild(std::move(row));
      scroll->content()->addChild(std::move(area));
    }
    return scroll;
  }

  void ViewerWindow::requestOutline() {
    m_helper->request({.op = preview::op::Outline}, {}, [this, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation || reply.msg.op != preview::op::OutlineReply || reply.data == nullptr) {
        return;
      }
      const auto bytes = reply.data->bytes();
      constexpr std::size_t kRow = 3 * sizeof(std::uint32_t);
      const std::size_t count = reply.msg.b;
      std::size_t text = count * kRow;
      if (text > bytes.size()) {
        return;
      }
      std::vector<OutlineEntry> outline;
      outline.reserve(count);
      for (std::size_t r = 0; r < count; ++r) {
        std::uint32_t v[3];
        std::memcpy(v, bytes.data() + r * kRow, sizeof(v));
        if (v[2] > bytes.size() - text) {
          return;
        }
        OutlineEntry entry{.depth = v[0], .title = std::string(reinterpret_cast<const char*>(bytes.data()) + text, v[2])};
        if (v[1] < m_model.pageCount()) {
          entry.page = v[1];
        }
        text += v[2];
        outline.push_back(std::move(entry));
      }
      m_outline = std::move(outline);
      rebuild();
    });
  }

  std::unique_ptr<Node> ViewerWindow::buildThumbs(Renderer& renderer, float width) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = Style::spaceMd * s});
    scroll->setSize(width, 0.0F);
    scroll->content()->setGap(Style::spaceMd * s);
    scroll->content()->setAlign(FlexAlign::Center);
    const std::size_t current = currentPage();
    m_shownPage = current;
    for (std::size_t i = 0; i < m_thumbs.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = kThumbWidth * s;
      const float h = w * (box.y1 - box.y0) / std::max(1.0F, box.x1 - box.x0);
      auto thumb = ui::inputArea({.width = w, .height = h, .onClick = [this, i](const InputArea::PointerData&) {
        if (m_scroll != nullptr) {
          m_scroll->setScrollOffset(m_model.pageBox(i).y0);
          onScroll();
          rebuild();
        }
      }});
      auto frame = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1),
                            .border = colorSpecFromRole(i == current ? ColorRole::Accent : ColorRole::Separator),
                            .borderWidth = i == current ? 2.0F * s : 1.0F, .width = w, .height = h});
      frame->setParticipatesInLayout(false);
      m_thumbFrames[i] = frame.get();
      thumb->addChild(std::move(frame));
      auto image = ui::image({.fit = ImageFit::Contain, .width = w, .height = h});
      if (const TextureHandle texture = pageTexture(renderer, m_thumbs[i]); texture.valid()) {
        image->setExternalTexture(renderer, texture);
      }
      image->setParticipatesInLayout(false);
      m_thumbImages[i] = image.get();
      thumb->addChild(std::move(image));
      scroll->content()->addChild(std::move(thumb));
    }
    scroll->setOnScrollChanged([this](float) { requestThumbs(); });
    m_thumbScroll = scroll.get();
    return scroll;
  }

  void ViewerWindow::setTool(Tool tool) {
    m_tool = tool;
    m_selection.reset();
    m_notePoint.reset();
    rebuild();
  }

  void ViewerWindow::onPagePress(std::size_t page, float x, float y, bool pressed, std::uint32_t time) {
    if (m_stamp && stampPress(page, x, y, pressed)) {
      return;
    }
    switch (m_tool) {
    case Tool::Select:
    case Tool::Highlight:
      if (pressed) {
        // Presses within 400 ms and 4 points of the last one count up: glyphs, words, lines, then glyphs again.
        const bool again = m_lastClick.count > 0 && time - m_lastClick.time <= 400 && page == m_lastClick.page &&
                           std::abs(x - m_lastClick.x) <= 4.0F && std::abs(y - m_lastClick.y) <= 4.0F;
        m_lastClick = {.time = time, .page = page, .x = x, .y = y, .count = again ? m_lastClick.count % 3 + 1 : 1};
        beginSelect(page, x, y);
        if (m_lastClick.count > 1) {
          m_selection->unit = m_lastClick.count == 2 ? preview::kSelectWord : preview::kSelectLine;
          requestSelect();
        }
      } else if (m_selecting) {
        m_selecting = false;
        // A click with no drag on a link follows it.
        const Link* link = m_tool == Tool::Select && m_selection && m_lastClick.count == 1 &&
                                   std::abs(x - m_selection->x0) <= 2.0F && std::abs(y - m_selection->y0) <= 2.0F
                               ? linkAt(page, m_selection->x0, m_selection->y0)
                               : nullptr;
        if (link != nullptr) {
          m_selection.reset();
          fillMarks(page);
          follow(*link);
          return;
        }
        m_highlightOnSelect = m_tool == Tool::Highlight;
        updateSelect(x, y);
      }
      break;
    case Tool::Note:
      if (pressed) {
        m_notePoint = {page, {x, y}};
        m_focusNote = true;
        rebuild();
      }
      break;
    case Tool::Draw:
      if (pressed) {
        m_drawing = true;
        m_strokePage = page;
        m_stroke = {{x, y}};
      } else if (m_drawing) {
        m_drawing = false;
        if (m_stroke.size() > 1) {
          m_pendingInk.push_back({.page = m_strokePage, .stroke = m_stroke});
          addAnnotation(m_strokePage, preview::kAnnotInk, kInkRgb, annot::inkBody({m_stroke}));
        }
        m_stroke.clear();
        fillMarks(m_strokePage);
        m_window.invalidate();
      }
      break;
    case Tool::Sign:
      if (pressed) {
        placeSignature(page, x, y);
      }
      break;
    }
  }

  void ViewerWindow::onPageMotion(float x, float y) {
    if (m_stamp && m_stamp->drag != StampFocus::Drag::None) {
      StampFocus& st = *m_stamp;
      const float dx = x - st.grab.x;
      const float dy = y - st.grab.y;
      if (st.drag == StampFocus::Drag::Move) {
        st.box = {st.at.x0 + dx, st.at.y0 + dy, st.at.x1 + dx, st.at.y1 + dy};
      } else {
        const float w0 = st.at.x1 - st.at.x0;
        const float w = std::max(kStampMinWidth, w0 + dx);
        st.box = {st.at.x0, st.at.y0, st.at.x0 + w, st.at.y0 + w * (st.at.y1 - st.at.y0) / w0};
      }
      fillMarks(st.page);
      m_window.invalidate();
      return;
    }
    if (m_tool == Tool::Draw && m_drawing) {
      // Points closer than half a point add nothing to the stroke.
      if (const auto& last = m_stroke.back(); std::hypot(x - last.x, y - last.y) >= 0.5F && m_stroke.size() < 8192) {
        m_stroke.push_back({x, y});
        if (m_strokePage < m_markLayers.size() && m_markLayers[m_strokePage] != nullptr) {
          addInkSegment(m_markLayers[m_strokePage], m_stroke[m_stroke.size() - 2], m_stroke.back());
        }
        m_window.invalidate();
      }
    } else if (m_selecting) {
      updateSelect(x, y);
    }
  }

  bool ViewerWindow::stampPress(std::size_t page, float x, float y, bool pressed) {
    StampFocus& st = *m_stamp;
    if (!pressed) {
      if (st.drag == StampFocus::Drag::None) {
        return false;
      }
      st.drag = StampFocus::Drag::None;
      if (st.box.x0 != st.at.x0 || st.box.y0 != st.at.y0 || st.box.x1 != st.at.x1) {
        moveStamp(st.box);
      }
      return true;
    }
    const float handle = kStampHandle * m_window.uiScale() / m_model.zoom();
    const bool onHandle = std::abs(x - st.box.x1) <= handle && std::abs(y - st.box.y1) <= handle;
    const bool inside = x >= st.box.x0 && x <= st.box.x1 && y >= st.box.y0 && y <= st.box.y1;
    if (page != st.page || (!onHandle && !inside)) {
      const std::size_t was = st.page;
      m_stamp.reset();
      fillMarks(was);
      m_window.invalidate();
      return false;
    }
    st.drag = onHandle ? StampFocus::Drag::Resize : StampFocus::Drag::Move;
    st.grab = {x, y};
    return true;
  }

  void ViewerWindow::moveStamp(const RectF& box) {
    const auto it = std::ranges::find(m_edits, m_stamp->id, &Edit::id);
    if (it == m_edits.end() || m_saving || !m_helper) {
      m_stamp->box = m_stamp->at;
      fillMarks(m_stamp->page);
      return;
    }
    // The file on disk no longer matches once an edit it holds changes.
    if (m_savedDepth > static_cast<std::size_t>(it - m_edits.begin())) {
      m_savedDepth.reset();
    }
    Edit moved = *it;
    annot::setStampBox(moved.body, box);
    m_undone.clear();
    m_helper->request({.op = preview::op::AnnotRemove, .b = moved.id},
                      [this, generation = m_generation](const preview::HelperReply& reply) {
                        if (generation == m_generation && reply.msg.op == preview::op::AnnotReply) {
                          rerenderPage(reply.msg.a);
                        }
                      });
    m_stamp->at = box;
    const std::uint32_t old = moved.id;
    sendAnnotation(std::move(moved), old);
    updateTitle();
  }

  void ViewerWindow::placeSignature(std::size_t page, float x, float y) {
    const auto sig = annot::loadSignature();
    if (!sig) {
      m_pendingSign = {page, {x, y}};
      m_padStrokes.clear();
      m_signing = true;
      rebuild();
      return;
    }
    // 120 pt wide, the image's aspect, centred on the click.
    const float w = 120.0F;
    const float h = w * static_cast<float>(sig->height) / static_cast<float>(sig->width);
    addAnnotation(page, preview::kAnnotStamp, 0,
                  annot::stampBody({x - w / 2.0F, y - h / 2.0F, x + w / 2.0F, y + h / 2.0F}, sig->width, sig->height,
                                   sig->pixels));
  }

  void ViewerWindow::addAnnotation(std::size_t page, std::uint32_t kind, std::uint32_t rgb,
                                   std::vector<std::uint8_t> body) {
    m_undone.clear();
    sendAnnotation({.page = page, .kind = kind, .rgb = rgb, .body = std::move(body)});
  }

  void ViewerWindow::sendAnnotation(Edit edit, std::optional<std::uint32_t> replaces) {
    if (!m_helper) {
      return;
    }
    const std::size_t page = edit.page;
    const std::uint32_t kind = edit.kind;
    const int fd = annot::sealedMemfd(edit.body);
    if (fd < 0) {
      kLog.warn("cannot pass the annotation to the helper");
      return;
    }
    m_helper->request(
        {.op = preview::op::AnnotAdd, .a = static_cast<std::uint32_t>(page), .b = kind, .c = edit.rgb}, fd,
        [this, page, kind, edit = std::move(edit), replaces, generation = m_generation](const preview::HelperReply& reply) mutable {
          if (generation != m_generation) {
            return;
          }
          if (reply.msg.op != preview::op::AnnotReply) {
            kLog.warn("annotation {} on page {} refused", kind, page + 1);
            if (std::erase_if(m_pendingInk, [&](const PendingInk& ink) { return ink.page == page && !ink.shownAfter; })
                > 0) {
              fillMarks(page);
              m_window.invalidate();
            }
            return;
          }
          edit.id = reply.msg.b;
          if (kind == preview::kAnnotStamp) {
            m_stamp = StampFocus{.id = edit.id, .page = page, .at = annot::stampBox(edit.body), .box = annot::stampBox(edit.body)};
          }
          const auto old = replaces ? std::ranges::find(m_edits, *replaces, &Edit::id) : m_edits.end();
          if (old != m_edits.end()) {
            *old = std::move(edit);
          } else {
            m_edits.push_back(std::move(edit));
          }
          for (PendingInk& ink : m_pendingInk) {
            if (ink.page == page && !ink.shownAfter) {
              ink.shownAfter = m_renderSeq; // a render sent after this one shows the stroke
            }
          }
          kLog.info("annotated page {} kind {} ({} edits)", page + 1, kind, m_edits.size());
          updateTitle();
          rerenderPage(page);
        }
    );
    close(fd);
  }

  void ViewerWindow::rerenderPage(std::size_t page) {
    if (page < m_pages.size()) {
      m_pages[page].requestedWidth = 0;
      m_thumbs[page].requestedWidth = 0;
      // Its tiles render again with the edit.
      for (auto& [key, tile] : m_tiles) {
        if (tile.page == page) {
          tile.pageWidth = 0;
        }
      }
    }
    requestVisiblePages();
  }

  void ViewerWindow::undo() {
    if (m_edits.empty() || m_saving || !m_helper) {
      return;
    }
    const std::uint32_t id = m_edits.back().id;
    if (m_stamp && m_stamp->id == id) {
      m_stamp.reset();
    }
    m_undone.push_back(std::move(m_edits.back()));
    m_edits.pop_back();
    if (m_savedDepth > m_edits.size()) {
      m_savedDepth.reset();
    }
    updateTitle();
    m_helper->request({.op = preview::op::AnnotRemove, .b = id},
                      [this, generation = m_generation](const preview::HelperReply& reply) {
                        if (generation != m_generation || reply.msg.op != preview::op::AnnotReply) {
                          return;
                        }
                        rerenderPage(reply.msg.a);
                      });
  }

  void ViewerWindow::redo() {
    if (m_undone.empty() || m_saving || !m_helper) {
      return;
    }
    Edit edit = std::move(m_undone.back());
    m_undone.pop_back();
    sendAnnotation(std::move(edit));
  }

  namespace {
    // A save's temp file and fd copy: released by the save reply, or here when the reply never comes (the window
    // closed and dropped its helper with the save out).
    struct SaveTemp {
      int keep;
      std::string path; // empty once renamed or removed
      SaveTemp(int fd, std::string tmp) : keep(fd), path(std::move(tmp)) {}
      SaveTemp(const SaveTemp&) = delete;
      SaveTemp& operator=(const SaveTemp&) = delete;
      ~SaveTemp() {
        if (keep >= 0) {
          close(keep);
        }
        if (!path.empty()) {
          std::error_code ec;
          std::filesystem::remove(path, ec);
        }
      }
    };
  } // namespace

  void ViewerWindow::saveThen(std::function<void()> then) {
    if (!unsaved() || m_saving || !m_helper) {
      return;
    }
    // A temp file next to the original, renamed over it once complete: a failed save leaves the original intact.
    // The real file behind a symlink is the one replaced; the link stays a link.
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::canonical(m_path, ec);
    struct stat st{};
    if (ec || stat(target.c_str(), &st) != 0) {
      kLog.warn("cannot save {}: {}", m_path.string(), ec ? ec.message() : std::strerror(errno));
      return;
    }
    std::string tmp = (target.parent_path() / ("." + target.filename().string() + ".XXXXXX")).string();
    const int fd = mkostemp(tmp.data(), O_CLOEXEC);
    if (fd < 0) {
      kLog.warn("cannot save {}: {}", m_path.string(), std::strerror(errno));
      return;
    }
    // Same mode and owner as the original (mkostemp makes 0600); the owner change works only for our own files.
    (void)fchmod(fd, st.st_mode & 07777);
    (void)fchown(fd, st.st_uid, st.st_gid);
    // poppler closes the fd it gets; this copy is for the fsync once it reports done.
    auto temp = std::make_shared<SaveTemp>(fcntl(fd, F_DUPFD_CLOEXEC, 0), tmp);
    m_saving = true;
    m_helper->request(
        {.op = preview::op::Save}, fd,
        [this, temp, target, depth = m_edits.size(), then = std::move(then), generation = m_generation](const preview::HelperReply& reply) {
          std::error_code err;
          const bool written = reply.msg.op == preview::op::Saved && temp->keep >= 0 && fsync(temp->keep) == 0;
          if (temp->keep >= 0) {
            close(std::exchange(temp->keep, -1));
          }
          const std::string tmp = std::exchange(temp->path, {});
          if (!written) {
            std::filesystem::remove(tmp, err);
          } else {
            std::filesystem::rename(tmp, target, err);
            if (err) {
              std::filesystem::remove(tmp, err);
            } else if (const int dir = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
              // The rename itself reaches the disk with the directory.
              fsync(dir);
              close(dir);
            }
          }
          if (generation != m_generation) {
            return;
          }
          m_saving = false;
          if (!written || err) {
            kLog.warn("saving {} failed", m_path.string());
            return;
          }
          m_savedDepth = depth;
          kLog.info("saved {}", m_path.string());
          updateTitle();
          if (then && !unsaved()) {
            then();
          }
        }
    );
    close(fd);
  }

  void ViewerWindow::updateTitle() {
    if (std::string now = (unsaved() ? "* " : "") + title(); now != m_shownTitle) {
      m_shownTitle = std::move(now);
      if (m_titleChanged) {
        m_titleChanged();
      }
    }
  }

  void ViewerWindow::detach() {
    forgetNodes();
    forgetCaption();
    forgetSidebar();
    m_window.animations().cancel(m_zoomAnim);
    m_zoomAnim = 0;
    m_zoomFrom = 1.0F;
  }

  void ViewerWindow::askSaveBeforeClose(std::function<void()> close) {
    m_window.presentAlert(
        {.title = i18n::tr("viewer.save.title", "name", m_path.filename().string()),
         .titleEllipsize = TextEllipsize::Middle,
         .body = i18n::tr("viewer.save.body"),
         .buttons = {{.text = i18n::tr("viewer.commands.save"), .variant = ButtonVariant::Primary,
                      .onClick = [this, close] {
                        m_window.dismissSheet();
                        saveThen(close);
                      }},
                     {.text = i18n::tr("viewer.save.discard"), .variant = ButtonVariant::Destructive,
                      .onClick = [this, close] {
                        m_window.dismissSheet();
                        close();
                      }},
                     {.text = i18n::tr("common.actions.cancel"), .onClick = [this] { m_window.dismissSheet(); }}}},
        [this] { m_window.dismissSheet(); });
  }

  std::unique_ptr<Node> ViewerWindow::buildSignaturePad() {
    const float s = m_window.uiScale();
    constexpr float kPadWidth = 480.0F;
    constexpr float kPadHeight = 200.0F;
    auto sheet = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * s,
                             .flexGrow = 1.0F});
    sheet->addChild(ui::label({.text = i18n::tr("viewer.signature.title"), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1)}));
    auto pad = ui::inputArea({.cursorShape = ::CursorShape::Crosshair, .width = kPadWidth * s, .height = kPadHeight * s});
    pad->setClipChildren(true);
    auto paper = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .border = colorSpecFromRole(ColorRole::Separator),
                          .borderWidth = 1.0F, .radius = Style::scaledRadiusMd(s), .width = kPadWidth * s,
                          .height = kPadHeight * s});
    paper->setParticipatesInLayout(false);
    pad->addChild(std::move(paper));
    for (const auto& stroke : m_padStrokes) {
      for (const annot::Point& p : stroke) {
        auto dot = ui::box({.fill = colorSpecFromRole(ColorRole::Label1), .radius = 1.5F * s, .width = 3.0F * s,
                            .height = 3.0F * s});
        dot->setParticipatesInLayout(false);
        dot->setPosition((p.x - 1.5F) * s, (p.y - 1.5F) * s);
        pad->addChild(std::move(dot));
      }
    }
    pad->setOnPress([this, s](const InputArea::PointerData& d) {
      if (d.button != BTN_LEFT) {
        return;
      }
      m_padDrawing = d.pressed;
      if (d.pressed) {
        m_padStrokes.push_back({{d.localX / s, d.localY / s}});
        rebuild();
      }
    });
    pad->setOnMotion([this, s](const InputArea::PointerData& d) {
      if (m_padDrawing && !m_padStrokes.empty() && m_padStrokes.back().size() < 4096) {
        m_padStrokes.back().push_back({d.localX / s, d.localY / s});
        rebuild();
      }
    });
    sheet->addChild(std::move(pad));
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s});
    buttons->addChild(ui::button({.text = i18n::tr("viewer.signature.clear"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_padStrokes.clear();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = i18n::tr("common.actions.cancel"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_signing = false;
      m_pendingSign.reset();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = i18n::tr("viewer.commands.save"), .enabled = !m_padStrokes.empty(),
                                  .variant = ButtonVariant::Primary, .onClick = [this, kPadWidth, kPadHeight] {
      m_signing = false;
      if (annot::saveSignature(m_padStrokes, static_cast<std::uint32_t>(kPadWidth), static_cast<std::uint32_t>(kPadHeight))
          && m_pendingSign) {
        placeSignature(m_pendingSign->first, m_pendingSign->second.x, m_pendingSign->second.y);
      }
      m_pendingSign.reset();
      rebuild();
    }}));
    sheet->addChild(std::move(buttons));
    return sheet;
  }

  // Rulebook "empty": symbol 48, 16, title-3, 4, body (2 lines), 16, one regular button.
  std::unique_ptr<Node> ViewerWindow::buildMessage(const std::string& title, const std::string& caption) {
    const float s = m_window.uiScale();
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 0.0F, .flexGrow = 1.0F});
    auto column = ui::column({.align = FlexAlign::Center, .gap = 0.0F, .maxWidth = Tokens::componentEmptyMaxWidth * s});
    column->addChild(ui::glyph({.glyph = std::string("file"), .glyphSize = Tokens::componentEmptySymbol * s,
                                .color = colorSpecFromRole(ColorRole::Label3)}));
    column->addChild(ui::node({.height = Tokens::spaceP16 * s}));
    auto heading = ui::label({.text = title, .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                              .textAlign = TextAlign::Center, .ellipsize = TextEllipsize::Middle});
    heading->setLineHeight(Tokens::typeTitle3.line * s);
    column->addChild(std::move(heading));
    column->addChild(ui::node({.height = Tokens::spaceP4 * s}));
    auto body = ui::label({.text = caption, .fontSize = Tokens::typeParagraph.size * s,
                           .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2,
                           .textAlign = TextAlign::Center, .ellipsize = TextEllipsize::End});
    body->setLineHeight(Tokens::typeParagraph.line * s);
    column->addChild(std::move(body));
    column->addChild(ui::node({.height = Tokens::spaceP16 * s}));
    column->addChild(ui::button({.text = i18n::tr("viewer.commands.open"), .onClick = [this] { m_window.commands().run("file.open"); }}));
    box->addChild(std::move(column));
    return box;
  }

} // namespace viewer
