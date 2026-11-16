#include "viewer_window.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/text_clipboard.h"
#include "ui/controls/input.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace viewer {

  namespace {
    constexpr Logger kLog("viewer");
    constexpr float kToolbarHeight = 44.0F;
    constexpr float kSidebarWidth = 148.0F;
    constexpr float kThumbWidth = 104.0F;
    constexpr float kPagePadding = 24.0F;
    constexpr std::uint32_t kMaxRenderWidth = 8192;
    // Device px: a whole page renders at most this wide; past it, kTile x kTile tiles show the part in view.
    constexpr std::uint32_t kBaseWidth = 2048;
    constexpr std::uint32_t kTile = 512;
    constexpr std::uint32_t kInkRgb = 0x1f5fd6;
    constexpr std::uint32_t kNoteRgb = 0xffd400;
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

    std::vector<std::uint8_t> areaPayload(float x0, float y0, float x1, float y1) {
      std::vector<std::uint8_t> out(16);
      const float v[4] = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
      std::memcpy(out.data(), v, sizeof(v));
      return out;
    }
  } // namespace

  ViewerWindow::ViewerWindow(AppLoop& loop, AppWindow& window) : m_loop(loop), m_window(window) {
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) {
      return build(renderer, width, height);
    });
    m_window.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_window.setCloseGuard([this] {
      if (!unsaved()) {
        return true;
      }
      askSaveBeforeClose();
      return false;
    });
    // A rebuilt ScrollView clamps its offset to its content, which is empty until the first layout: restore after it.
    m_window.setLayoutCallback([this] {
      // Once per rebuild: a document shorter than the old offset clamps, and must not loop.
      // A placed note's field takes the keys at once.
      if (m_focusNote && m_noteInput != nullptr) {
        m_focusNote = false;
        m_window.input().setFocus(m_noteInput->inputArea());
      }
      if (m_restoreScroll && m_scroll != nullptr) {
        m_restoreScroll = false;
        if (m_scroll->scrollOffset() != m_scrollOffset) {
          m_scroll->setScrollOffset(m_scrollOffset);
          m_window.invalidate();
        }
      }
    });
  }

  ViewerWindow::~ViewerWindow() {
    if (unsaved()) {
      kLog.info("unsaved edits dropped: {}", m_path.string());
    }
    m_decoder.cancel();
    dropHelper();
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
    m_inFlight = 0;
    m_pendingInk.clear();
    m_image = {};
    m_selection.reset();
    m_searchText.clear();
    m_model.clearSearch();
    m_scrollOffset = 0.0F;
    m_hOffset = 0.0F;
    m_error.clear();
    m_window.setTitle(path.filename().string());
    const auto kind = preview::kindForContentType(contentTypeOf(path));
    if (kind == preview::FileKind::Pdf) {
      openPdf();
    } else if (kind == preview::FileKind::Image) {
      loadFolder();
      openImage();
    } else {
      fail("This file type cannot be shown");
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
      fail("This file cannot be opened");
      return;
    }
    m_loop.addSource(m_helper.get());
    m_helper->request({.op = preview::op::Info}, [this, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      if (reply.msg.op != preview::op::InfoReply || reply.msg.b == 0) {
        DeferredCall::callLater([this, generation] {
          if (generation == m_generation) {
            dropHelper();
          }
        });
        fail("This file cannot be opened");
        return;
      }
      m_model.reset(std::min<std::uint32_t>(reply.msg.b, 100000), static_cast<float>(reply.msg.c) / 100.0F,
                    static_cast<float>(reply.msg.d) / 100.0F);
      m_pages.assign(m_model.pageCount(), {});
      m_thumbs.assign(m_model.pageCount(), {});
      m_mode = Mode::Pdf;
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
        fail("This image cannot be opened");
        return;
      }
      m_image = std::move(pixels);
      m_mode = Mode::Image;
      kLog.info("opened image {}x{}", m_image.width, m_image.height);
      rebuild();
    });
  }

  void ViewerWindow::step(int direction) {
    if (m_mode == Mode::Image || (m_mode == Mode::Loading && !m_folder.empty() && !m_helper)) {
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
      m_window.setTitle(path.filename().string());
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

  std::size_t ViewerWindow::currentPage() const {
    return m_scroll != nullptr ? m_model.pageAt(m_scroll->scrollOffset() + 1.0F) : 0;
  }

  void ViewerWindow::onScroll() {
    if (m_scroll != nullptr) {
      m_scrollOffset = m_scroll->scrollOffset();
    }
    requestVisiblePages();
    refreshLabels();
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
    // Two in flight: the helper works on one while the next waits, and a scroll never queues stale work behind it.
    constexpr int kInFlight = 2;
    while (m_inFlight < kInFlight && !m_jobs.empty() && m_helper) {
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
    ++m_inFlight;
    preview::HelperMsg msg{.op = preview::op::RenderTile, .a = static_cast<std::uint32_t>(job.page), .b = job.width,
                           .c = job.x, .d = job.y, .e = job.w << 16U | job.h};
    m_helper->request(msg, [this, job, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      --m_inFlight;
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
      mark(hits[k], ColorRole::Accent, isCurrent ? 0.55F : 0.25F);
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
    if (m_positionLabel != nullptr) {
      (void)m_positionLabel->setText(std::format("{} / {}", page + 1, m_model.pageCount()));
    }
    if (m_countLabel != nullptr) {
      (void)m_countLabel->setText(m_model.countText());
    }
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
      DeferredCall::callLater([this, generation = m_generation] {
        if (generation == m_generation) {
          requestVisiblePages();
        }
      });
    }
    return page.texture;
  }

  void ViewerWindow::requestRender(std::vector<Page>& cache, std::size_t page, std::uint32_t width) {
    cache[page].requestedWidth = width;
    ++m_inFlight;
    const std::uint64_t seq = ++m_renderSeq;
    m_helper->request(
        {.op = preview::op::Render, .a = static_cast<std::uint32_t>(page), .b = width, .c = kMaxPageHeight},
        [this, &cache, page, width, seq, generation = m_generation](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          --m_inFlight;
          // Sent last, so it runs after this reply is stored.
          DeferredCall::callLater([this, generation] {
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
    m_searchText = capQuery(query);
    if (m_searchText.empty()) {
      m_model.clearSearch();
    } else {
      m_model.startSearch(m_searchText);
      // Its own counter: a new search drops the old search's finds, not the edits, renders or saves in flight.
      ++m_searchGeneration;
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
            kLog.info("search \"{}\" {}", m_searchText, m_model.countText());
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
        {.op = preview::op::Select, .a = static_cast<std::uint32_t>(s.page), .b = preview::kSelectGlyph},
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

  void ViewerWindow::copySelection() {
    TextClipboard* clipboard = Input::textClipboard();
    if (m_selection && !m_selection->text.empty() && clipboard != nullptr) {
      clipboard->setClipboardText(m_selection->text);
      kLog.info("copied {} bytes", m_selection->text.size());
    }
  }

  void ViewerWindow::searchFor(const std::string& query) {
    if (m_searchInput != nullptr) {
      m_searchInput->setValue(query);
    }
    search(query);
  }

  void ViewerWindow::focusSearch() {
    if (m_searchInput != nullptr) {
      m_window.input().setFocus(m_searchInput->inputArea());
    }
  }

  void ViewerWindow::zoomIn() { zoomTo([this] { m_model.zoomBy(1.25F); }); }

  void ViewerWindow::zoomOut() { zoomTo([this] { m_model.zoomBy(0.8F); }); }

  void ViewerWindow::zoomFit() { zoomTo([this] { m_model.fitWidth(); }); }

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
    rebuild();
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
    if ((m_loop.wayland().keyboardModifiers() & KeyMod::Ctrl) == 0 || data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    // One wheel notch (value 15) zooms by 10 %; a touchpad scales in proportion.
    const float factor = std::pow(1.1F, -static_cast<float>(data.axisValue) / 15.0F);
    zoomTo([this, factor] { m_model.zoomBy(factor); }, data.localX, data.localY);
    return true;
  }

  void ViewerWindow::toggleSidebar() {
    m_sidebar = !m_sidebar;
    rebuild();
  }

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
    case XKB_KEY_Return:
      if (!m_searchText.empty()) {
        moveHit(!shift);
        return true;
      }
      return false;
    case XKB_KEY_Escape:
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

  std::unique_ptr<Node> ViewerWindow::build(Renderer& renderer, float width, float height) {
    const float s = m_window.uiScale();
    for (TextureHandle& texture : m_dropped) {
      renderer.textureManager().unload(texture);
    }
    m_dropped.clear();
    m_scroll = nullptr;
    m_hscroll = nullptr;
    m_column = nullptr;
    m_thumbScroll = nullptr;
    m_searchInput = nullptr;
    m_noteInput = nullptr;
    m_positionLabel = nullptr;
    m_countLabel = nullptr;
    // The old tree is gone: its tile nodes with it.
    for (auto& [key, tile] : m_tiles) {
      tile.node = nullptr;
    }
    m_tileLayers.assign(m_pages.size(), nullptr);
    m_pageImages.assign(m_pages.size(), nullptr);
    m_thumbImages.assign(m_thumbs.size(), nullptr);
    m_thumbFrames.assign(m_thumbs.size(), nullptr);
    m_markLayers.assign(m_pages.size(), nullptr);
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    // Full screen is immersive: the page column alone.
    const bool immersive = m_window.fullscreen() && m_mode == Mode::Pdf;
    if (!immersive) {
      root->addChild(buildToolbar(width));
      root->addChild(ui::separator({}));
    }
    switch (m_mode) {
    case Mode::Empty:
      root->addChild(buildMessage("Open a document or image", "or drop a file here"));
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
      auto body = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
      if (m_sidebar && !immersive) {
        body->addChild(buildSidebar(renderer));
        body->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      }
      body->addChild(buildPages(renderer));
      root->addChild(std::move(body));
      break;
    }
    }
    if (m_mode == Mode::Pdf) {
      DeferredCall::callLater([this, generation = m_generation] {
        if (generation == m_generation) {
          requestVisiblePages();
        }
      });
    }
    return root;
  }

  std::unique_ptr<Node> ViewerWindow::buildToolbar(float width) {
    const float s = m_window.uiScale();
    auto bar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                        .height = kToolbarHeight * s});
    if (m_mode == Mode::Pdf) {
      bar->addChild(ui::button({.glyph = std::string("layout-sidebar"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Sidebar"), .onClick = [this] { toggleSidebar(); }}));
    }
    bar->addChild(ui::label({.text = m_path.empty() ? std::string("Viewer") : m_path.filename().string(),
                             .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1),
                             .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    std::string position;
    if (m_mode == Mode::Pdf) {
      position = std::format("{} / {}", currentPage() + 1, m_model.pageCount());
    } else if (m_mode == Mode::Image && m_folder.size() > 1) {
      position = std::format("{} of {}", m_folderIndex + 1, m_folder.size());
    }
    if (!position.empty()) {
      auto label = ui::label({.text = position, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::Label2)});
      if (m_mode == Mode::Pdf) {
        m_positionLabel = label.get();
      }
      bar->addChild(std::move(label));
    }
    if (m_mode == Mode::Pdf) {
      bar->addChild(ui::button({.glyph = std::string("minus"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Zoom Out"), .onClick = [this] { zoomOut(); }}));
      bar->addChild(ui::button({.text = std::format("{}%", std::lround(m_model.zoom() * 100.0F)),
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("Fit Width"),
                                .onClick = [this] { zoomFit(); }}));
      bar->addChild(ui::button({.glyph = std::string("plus"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Zoom In"), .onClick = [this] { zoomIn(); }}));
      const auto tool = [&](Tool t, const char* glyph, const char* tip) {
        bar->addChild(ui::button({.glyph = std::string(glyph), .selected = m_tool == t,
                                  .variant = m_tool == t ? ButtonVariant::TabActive : ButtonVariant::Ghost,
                                  .tooltip = std::string(tip), .onClick = [this, t] { setTool(t); }}));
      };
      tool(Tool::Select, "cursor-text", "Select Text");
      tool(Tool::Highlight, "highlight", "Highlight");
      tool(Tool::Note, "note", "Note");
      tool(Tool::Draw, "pencil", "Draw");
      tool(Tool::Sign, "signature", "Sign");
      if (m_notePoint) {
        // The note field takes the search field's place, so the tools do not move.
        auto note = ui::input({
            .placeholder = std::string("Note"),
            .width = std::min(240.0F * s, width / 3.0F),
            .onSubmit =
                [this](const std::string& text) {
                  if (m_notePoint && !text.empty()) {
                    addAnnotation(m_notePoint->first, preview::kAnnotNote, kNoteRgb,
                                  annot::noteBody(m_notePoint->second, text));
                  }
                  m_notePoint.reset();
                  rebuild();
                },
        });
        m_noteInput = note.get();
        bar->addChild(std::move(note));
        return bar;
      }
      auto field = ui::input({
          .value = m_searchText,
          .placeholder = std::string("Search"),
          .clearButtonEnabled = true,
          .width = std::min(240.0F * s, width / 3.0F),
          .onChange = [this](const std::string& text) { search(text); },
          .onSubmit = [this](const std::string&) { moveHit(true); },
      });
      // A rebuild hands the keyboard focus to the new field by this key.
      field->inputArea()->setTabFocusKey("search");
      m_searchInput = field.get();
      bar->addChild(std::move(field));
      auto count = ui::label({.text = m_model.countText(), .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::Label2)});
      m_countLabel = count.get();
      bar->addChild(std::move(count));
    }
    return bar;
  }

  std::unique_ptr<Node> ViewerWindow::buildPages(Renderer& renderer) {
    const float s = m_window.uiScale();
    const float pad = kPagePadding * s;
    auto scroll = ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = pad});
    scroll->setFlexGrow(1.0F);
    // Fit-width uses the area inside the padding; the sidebar and the window size change it.
    const bool sidebar = m_sidebar && !m_window.fullscreen();
    const float area = m_window.contentRect().width - (sidebar ? kSidebarWidth * s : 0.0F);
    const float viewport = area - 2.0F * pad;
    m_model.setViewportWidth(std::max(100.0F, viewport));
    // A page wider than the view pans sideways in its own scroll view; vertical scrolling passes through it.
    auto hscroll = ui::scrollView({.viewportPaddingH = pad, .viewportPaddingV = 0.0F});
    hscroll->setOrientation(ScrollOrientation::Horizontal);
    hscroll->setScrollbarVisible(false);
    hscroll->setSize(area, m_model.contentHeight());
    auto columnNode = ui::column({.align = FlexAlign::Center, .gap = DocumentModel::kPageGap,
                                  .width = std::max(viewport, widestPage())});
    Node* column = columnNode.get();
    const float zoom = m_model.zoom();
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = box.x1 - box.x0;
      const float h = box.y1 - box.y0;
      auto sheet = ui::inputArea({.cursorShape = 9 /* text */, .width = w, .height = h});
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
      sheet->setOnPress([this, i, zoom](const InputArea::PointerData& d) {
        if (d.button == BTN_LEFT) {
          onPagePress(i, d.localX / zoom, d.localY / zoom, d.pressed);
        }
      });
      sheet->setOnMotion([this, zoom](const InputArea::PointerData& d) { onPageMotion(d.localX / zoom, d.localY / zoom); });
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

  std::unique_ptr<Node> ViewerWindow::buildSidebar(Renderer& renderer) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = Style::spaceMd * s, .viewportPaddingV = Style::spaceMd * s});
    scroll->setSize(kSidebarWidth * s, 0.0F);
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

  void ViewerWindow::onPagePress(std::size_t page, float x, float y, bool pressed) {
    switch (m_tool) {
    case Tool::Select:
    case Tool::Highlight:
      if (pressed) {
        beginSelect(page, x, y);
      } else if (m_selecting) {
        m_selecting = false;
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

  void ViewerWindow::sendAnnotation(Edit edit) {
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
        [this, page, kind, edit = std::move(edit), generation = m_generation](const preview::HelperReply& reply) mutable {
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
          m_edits.push_back(std::move(edit));
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
    const int keep = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    m_saving = true;
    m_helper->request(
        {.op = preview::op::Save}, fd,
        [this, tmp, target, keep, depth = m_edits.size(), then = std::move(then), generation = m_generation](const preview::HelperReply& reply) {
          std::error_code err;
          const bool written = reply.msg.op == preview::op::Saved && keep >= 0 && fsync(keep) == 0;
          if (keep >= 0) {
            close(keep);
          }
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
    m_window.setTitle((unsaved() ? "• " : "") + m_path.filename().string());
  }

  void ViewerWindow::askSaveBeforeClose() {
    m_window.presentSheet(
        [this](float s) {
          auto box = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP12 * s,
                                 .padding = Tokens::componentDialogPadding * s,
                                 .width = Tokens::componentDialogAlertWidth * s});
          box->addChild(ui::label({.text = std::format("Save changes to “{}”?", m_path.filename().string()),
                                   .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                                   .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                                   .ellipsize = TextEllipsize::Middle}));
          box->addChild(ui::label({.text = std::string("Your annotations are lost if you close without saving."),
                                   .fontSize = Tokens::typeBody.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                                   .maxLines = 3}));
          auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
          buttons->addChild(ui::button({.text = std::string("Don’t Save"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Destructive, .onClick = [this] {
                                          m_window.dismissSheet();
                                          m_window.finishClose();
                                        }}));
          buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Default, .onClick = [this] { m_window.dismissSheet(); }}));
          buttons->addChild(ui::button({.text = std::string("Save"), .fontSize = Tokens::typeBody.size * s,
                                        .variant = ButtonVariant::Primary, .onClick = [this] {
                                          m_window.dismissSheet();
                                          saveThen([this] { m_window.finishClose(); });
                                        }}));
          box->addChild(std::move(buttons));
          return box;
        },
        [this] { m_window.dismissSheet(); });
  }

  std::unique_ptr<Node> ViewerWindow::buildSignaturePad() {
    const float s = m_window.uiScale();
    constexpr float kPadWidth = 480.0F;
    constexpr float kPadHeight = 200.0F;
    auto sheet = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * s,
                             .flexGrow = 1.0F});
    sheet->addChild(ui::label({.text = std::string("Draw your signature"), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1)}));
    auto pad = ui::inputArea({.cursorShape = 2 /* crosshair */, .width = kPadWidth * s, .height = kPadHeight * s});
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
    buttons->addChild(ui::button({.text = std::string("Clear"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_padStrokes.clear();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_signing = false;
      m_pendingSign.reset();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = std::string("Save"), .enabled = !m_padStrokes.empty(),
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

  std::unique_ptr<Node> ViewerWindow::buildMessage(const std::string& title, const std::string& caption) {
    const float s = m_window.uiScale();
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                           .flexGrow = 1.0F});
    box->addChild(ui::label({.text = title, .fontSize = Style::fontSizeTitle * s,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                             .textAlign = TextAlign::Center}));
    box->addChild(ui::label({.text = caption, .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
    return box;
  }

} // namespace viewer
