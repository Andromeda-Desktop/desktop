#include "print_sheet.h"

#include "app/app_loop.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "design_tokens.h"
#include "helper_client.h"
#include "print_jobs.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <gio/gdesktopappinfo.h>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <thread>
#include <unistd.h>

namespace {
  constexpr Logger kLog("print-sheet");
  constexpr float kColumnWidth = 420.0F; // mockup §3 right column
  constexpr float kKeyWidth = 96.0F;     // mockup .opt .k
  constexpr int kPageRenderWidth = 640;  // preview pixels; the page shows at most ~300 pt high
  constexpr float kPageHeight = 300.0F;  // mockup .page

  std::string appName(const std::string& appId) {
    if (appId.empty())
      return "An app";
    std::string name = appId;
    if (GDesktopAppInfo* info = g_desktop_app_info_new((appId + ".desktop").c_str())) {
      name = g_app_info_get_name(G_APP_INFO(info));
      g_object_unref(info);
    }
    return name;
  }

  constexpr int kThumbRenderWidth = 120;
  constexpr float kThumbHeight = 48.0F; // mockup .thumbs
  constexpr int kThumbsVisible = 8;
  constexpr std::size_t kPageCache = 24;
  constexpr int kMaxPerSheet = 6;
  // The strip shows up to kThumbsVisible sheets of up to kMaxPerSheet pages; a smaller cache evicts a shown page,
  // whose render rebuilds and asks for it again.
  constexpr std::size_t kThumbCache = kThumbsVisible * kMaxPerSheet + kMaxPerSheet;

  // One sheet of paper: the pages placed on it in a cols × rows grid, each fitted into its cell. Uploads new pixels
  // on layout, where a Renderer is at hand.
  class SheetNode final : public Node {
  public:
    // A cell's texture is invalid while its page renders: the cell stays white.
    SheetNode(std::vector<TextureHandle> cells, int cols, int rows, float width, float height, float scale, bool ring)
        : m_cells(std::move(cells)), m_cols(cols), m_rows(rows) {
      if (ring) {
        const float r = 2.0F * scale;
        auto* box = static_cast<Box*>(addChild(ui::box({.border = colorSpecFromRole(ColorRole::Accent), .borderWidth = r,
                                                        .radius = 4.0F * scale})));
        box->setPosition(-r - 1.0F * scale, -r - 1.0F * scale);
        box->setSize(width + 2.0F * (r + 1.0F * scale), height + 2.0F * (r + 1.0F * scale));
        m_ring = box;
      }
      m_paper = static_cast<Box*>(addChild(ui::box({.fill = fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 1.0F)),
                                                    .border = colorSpecFromRole(ColorRole::Separator),
                                                    .borderWidth = std::max(1.0F, 0.5F * scale)})));
      m_paper->setSize(width, height);
      for (const TextureHandle& cell : m_cells) {
        auto* image = static_cast<Image*>(addChild(std::make_unique<Image>()));
        image->setFit(ImageFit::Contain);
        image->setVisible(cell.valid());
        m_images.push_back(image);
      }
      setSize(width, height);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_ring)
        m_ring->layout(renderer);
      m_paper->setSize(width(), height());
      m_paper->layout(renderer);
      const bool many = m_cells.size() > 1;
      const float margin = many ? std::min(width(), height()) * 0.04F : 0.0F;
      const float cw = (width() - 2.0F * margin - static_cast<float>(m_cols - 1) * margin) / static_cast<float>(m_cols);
      const float ch = (height() - 2.0F * margin - static_cast<float>(m_rows - 1) * margin) / static_cast<float>(m_rows);
      for (std::size_t i = 0; i < m_cells.size(); ++i) {
        if (!m_cells[i].valid())
          continue;
        Image* image = m_images[i];
        if (!m_bound)
          image->setExternalTexture(renderer, m_cells[i]);
        const int col = static_cast<int>(i) % m_cols;
        const int row = static_cast<int>(i) / m_cols;
        image->setPosition(margin + static_cast<float>(col) * (cw + margin),
                           margin + static_cast<float>(row) * (ch + margin));
        image->setSize(cw, ch);
        image->layout(renderer);
      }
      m_bound = true;
    }

  private:
    std::vector<TextureHandle> m_cells;
    std::vector<Image*> m_images;
    Box* m_ring = nullptr;
    Box* m_paper = nullptr;
    int m_cols = 1;
    int m_rows = 1;
    bool m_bound = false;
  };

  struct Grid {
    int cols = 1;
    int rows = 1;
  };

  Grid gridFor(int perSheet, bool landscapeSheet) {
    Grid g = perSheet == 2 ? Grid{2, 1} : perSheet == 4 ? Grid{2, 2} : perSheet == 6 ? Grid{3, 2} : Grid{1, 1};
    if (!landscapeSheet && g.cols > g.rows)
      std::swap(g.cols, g.rows);
    return g;
  }

  ColorSpec stateColour(PrinterState state) {
    switch (state) {
    case PrinterState::Ready:
    case PrinterState::Printing:
      return colorSpecFromRole(ColorRole::Success);
    case PrinterState::NeedsAttention:
      return colorSpecFromRole(ColorRole::Danger);
    case PrinterState::Offline:
      return colorSpecFromRole(ColorRole::Label3);
    }
    return colorSpecFromRole(ColorRole::Label3);
  }

  std::string stateText(const PrinterInfo& p) {
    switch (p.state) {
    case PrinterState::Ready:
      return p.reason.empty() ? std::string("Ready") : "Ready · " + p.reason;
    case PrinterState::Printing:
      return p.queuedJobs > 0 ? std::format("Printing · {} {} waiting", p.queuedJobs, p.queuedJobs == 1 ? "job" : "jobs")
                              : std::string("Printing");
    case PrinterState::NeedsAttention:
      return (p.reason.empty() ? std::string("Needs attention") : p.reason) + ". The job waits.";
    case PrinterState::Offline:
      return "Offline · the job prints when it is back";
    }
    return {};
  }
} // namespace

PrintSheet::PrintSheet(AppLoop& loop, PrintPortal::PrintRequest request, Callbacks callbacks)
    : m_loop(loop), m_request(std::move(request)), m_cb(std::move(callbacks)), m_options(m_request.remembered),
      m_appMedia(m_request.remembered.media) {
  m_options.ranges.reset();
  std::thread([alive = std::weak_ptr<bool>(m_alive), this] {
    auto printers = listPrinters();
    DeferredCall::callLater([alive, this, printers = std::move(printers)]() mutable {
      if (alive.expired())
        return;
      m_printers = std::move(printers);
      m_printersState = Printers::Loaded;
      std::size_t index = 0;
      for (std::size_t i = 0; i < m_printers.size(); ++i)
        if (m_printers[i].name == m_request.remembered.printer)
          index = i;
      if (!m_printers.empty())
        selectPrinter(index);
      if (m_cb.rebuild)
        m_cb.rebuild();
    });
  }).detach();
  startPreview();
}

PrintSheet::~PrintSheet() {
  *m_alive = false;
  if (m_helper)
    m_loop.removeSource(m_helper.get());
  if (!m_tempPath.empty())
    ::unlink(m_tempPath.c_str());
}

void PrintSheet::startPreview() {
  // The preview helper opens a path before its sandbox starts; the document lives in a private copy for that.
  const char* runtime = g_get_user_runtime_dir();
  m_tempPath = std::format("{}/andromeda-print-{}-{}.pdf", runtime, ::getpid(), reinterpret_cast<std::uintptr_t>(this));
  const int out = ::open(m_tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  bool copied = out >= 0;
  char buf[65536];
  for (off_t offset = 0; copied;) {
    const ssize_t n = ::pread(m_request.fd, buf, sizeof buf, offset);
    if (n <= 0) {
      copied = n == 0;
      break;
    }
    copied = ::write(out, buf, static_cast<std::size_t>(n)) == n;
    offset += n;
  }
  if (out >= 0)
    ::close(out);
  m_helper = copied ? preview::HelperClient::start(preview::HelperKind::Pdf, m_tempPath) : nullptr;
  if (!m_helper) {
    m_preview = Preview::Failed;
    return;
  }
  m_loop.addSource(m_helper.get());
  m_helper->request({.op = preview::op::Info}, [this, alive = std::weak_ptr<bool>(m_alive)](const preview::HelperReply& r) {
    if (alive.expired())
      return;
    if (r.msg.op != preview::op::InfoReply || r.msg.b == 0) {
      kLog.info("no preview for \"{}\"", m_request.title);
      m_preview = Preview::Failed;
    } else {
      m_pageCount = static_cast<int>(std::min<std::uint32_t>(r.msg.b, 100000));
      m_preview = Preview::Ready;
    }
    validateRange();
    if (m_cb.rebuild)
      m_cb.rebuild();
  });
}

void PrintSheet::requestPage(int page, Size size) {
  auto& cache = size == Size::Page ? m_pages : m_thumbs;
  if (!m_helper || page < 0 || page >= m_pageCount || cache.contains(page) || m_requested.contains({page, size}))
    return;
  m_requested.insert({page, size});
  const auto width = static_cast<std::uint32_t>(size == Size::Page ? kPageRenderWidth : kThumbRenderWidth);
  m_helper->request({.op = preview::op::Render, .a = static_cast<std::uint32_t>(page), .b = width, .c = width * 2},
                    [this, page, size, alive = std::weak_ptr<bool>(m_alive)](const preview::HelperReply& r) {
                      if (alive.expired())
                        return;
                      m_requested.erase({page, size});
                      auto& done = size == Size::Page ? m_pages : m_thumbs;
                      if (r.msg.op != preview::op::Page || !r.data) {
                        kLog.info("page {} of \"{}\" did not render", page + 1, m_request.title);
                        return;
                      }
                      auto px = std::make_shared<Pixels>();
                      const auto bytes = r.data->bytes();
                      px->bgra.assign(bytes.begin(), bytes.end());
                      px->width = static_cast<int>(r.msg.b);
                      px->height = static_cast<int>(r.msg.c);
                      px->stride = static_cast<int>(r.msg.d);
                      done[page] = std::move(px);
                      // Keep the pages nearest the shown sheet, by position in the pages to print (ascending).
                      const std::vector<int> order = pagesToPrint();
                      const auto position = [&order](int pg) {
                        return static_cast<int>(std::ranges::lower_bound(order, pg) - order.begin());
                      };
                      const int centre = m_sheet * perSheet();
                      while (done.size() > (size == Size::Page ? kPageCache : kThumbCache)) {
                        const auto far = std::ranges::max_element(done, {}, [&](const auto& kv) {
                          return std::ranges::binary_search(order, kv.first) ? std::abs(position(kv.first) - centre)
                                                                             : std::numeric_limits<int>::max();
                        });
                        if (far->second->texture.valid())
                          m_dropped.push_back(far->second->texture);
                        done.erase(far);
                      }
                      if (m_cb.rebuild)
                        m_cb.rebuild();
                    });
}

const PrinterInfo* PrintSheet::printer() const {
  return m_target == Target::Printer && m_printer < m_printers.size() ? &m_printers[m_printer] : nullptr;
}

void PrintSheet::selectPrinter(std::size_t index) {
  m_target = Target::Printer;
  m_printer = index;
  const PrinterInfo& p = m_printers[index];
  if (p.name != m_options.printer) {
    m_options = PrintOptions{.printer = p.name, .copies = m_options.copies, .collate = m_options.collate};
  }
  if (std::ranges::find(p.media, m_options.media) == p.media.end())
    m_options.media = std::ranges::find(p.media, m_appMedia) != p.media.end() ? m_appMedia : p.defaultMedia;
  if (!p.duplex)
    m_options.duplex = Duplex::Off;
  if (!p.colour)
    m_options.colour = false;
}

void PrintSheet::selectSaveAsPdf() {
  m_target = Target::SaveAsPdf;
  m_screen = Screen::Main;
}

bool PrintSheet::rangeValid() const { return m_allPages || (m_rangeError.empty() && !m_rangeText.empty()); }

// Without a page count (preview loading or failed) any page number passes; CUPS skips pages past the end.
int PrintSheet::pageLimit() const { return m_pageCount > 0 ? m_pageCount : std::numeric_limits<int>::max(); }

void PrintSheet::validateRange() {
  m_rangeError = m_rangeText.empty() ? std::string()
                                     : parsePageRanges(m_rangeText, pageLimit()).second.value_or(RangeError{}).message;
}

void PrintSheet::showRange() {
  const bool error = !m_allPages && !m_rangeError.empty();
  if (m_rangeInput)
    m_rangeInput->setInvalid(error);
  if (m_rangeNote) {
    m_rangeNoteLabel->setText(m_rangeError);
    m_rangeNote->setVisible(error);
    m_rangeNote->setParticipatesInLayout(error);
  }
  if (m_printButton && m_target == Target::Printer)
    m_printButton->setEnabled(!m_sent && printer() != nullptr && rangeValid());
}

int PrintSheet::perSheet() const { return m_target == Target::SaveAsPdf ? 1 : std::max(1, m_options.numberUp); }

std::vector<int> PrintSheet::pagesToPrint() const {
  std::vector<int> pages;
  if (m_target == Target::Printer && !m_allPages && rangeValid()) {
    for (const PageRange& r : parsePageRanges(m_rangeText, pageLimit()).first)
      for (int p = r.first; p <= std::min(r.last, m_pageCount); ++p)
        pages.push_back(p - 1);
    return pages;
  }
  for (int p = 0; p < m_pageCount; ++p)
    pages.push_back(p);
  return pages;
}

void PrintSheet::submit() {
  if (m_sent)
    return;
  if (m_target == Target::SaveAsPdf) {
    if (m_cb.savePdf)
      m_cb.savePdf();
    return;
  }
  const PrinterInfo* p = printer();
  if (!p || !rangeValid())
    return;
  m_options.ranges.reset();
  if (!m_allPages)
    m_options.ranges = parsePageRanges(m_rangeText, std::max(m_pageCount, 1)).first;
  m_sent = true;
  if (m_cb.print)
    m_cb.print(m_options, p->label, static_cast<int>(pagesToPrint().size()));
}

void PrintSheet::showSheet(int sheet) {
  m_sheet = sheet;
  if (m_cb.rebuild)
    m_cb.rebuild();
}

bool PrintSheet::firstKey(const KeyboardEvent& event, const InputArea* focused) {
  if (!event.pressed || event.modifiers != 0)
    return false;
  switch (event.sym) {
  case XKB_KEY_Escape:
    if (m_screen == Screen::More) {
      m_screen = Screen::Main;
      if (m_cb.rebuild)
        m_cb.rebuild();
    } else if (m_cb.cancel) {
      m_cb.cancel();
    }
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (focused != nullptr && focused->textInputClient() == nullptr)
      return false;
    submit();
    return true;
  default:
    return false;
  }
}

bool PrintSheet::key(const KeyboardEvent& event) {
  if (!event.pressed || (event.sym != XKB_KEY_Left && event.sym != XKB_KEY_Right))
    return false;
  const int pages = static_cast<int>(pagesToPrint().size());
  const int sheets = std::max(1, (pages + perSheet() - 1) / perSheet());
  if (sheets < 2)
    return false;
  showSheet(std::clamp(m_sheet + (event.sym == XKB_KEY_Left ? -1 : 1), 0, sheets - 1));
  return true;
}

std::unique_ptr<Node> PrintSheet::buildSheet(Renderer& renderer, int sheet, const std::vector<int>& pages, float height, Size size,
                                             bool selected) {
  const int n = perSheet();
  auto& cache = size == Size::Page ? m_pages : m_thumbs;
  std::vector<TextureHandle> cells;
  for (int i = sheet * n; i < std::min<int>((sheet + 1) * n, static_cast<int>(pages.size())); ++i) {
    const int page = pages[static_cast<std::size_t>(i)];
    requestPage(page, size);
    const auto it = cache.find(page);
    if (it == cache.end()) {
      cells.emplace_back();
      continue;
    }
    Pixels& px = *it->second;
    if (!px.texture.valid()) {
      px.texture = renderer.textureManager().loadFromRaw(px.bgra.data(), px.bgra.size(), px.width, px.height,
                                                         px.stride, PixmapFormat::BGRA);
      px.bgra = {};
    }
    cells.push_back(px.texture);
  }
  // The page shape from the first rendered page, else the app's paper, else A4.
  float pageAspect = 0.707F;
  for (const auto& px : {m_pages.empty() ? nullptr : m_pages.begin()->second,
                         m_thumbs.empty() ? nullptr : m_thumbs.begin()->second}) {
    if (px && px->height > 0) {
      pageAspect = static_cast<float>(px->width) / static_cast<float>(px->height);
      break;
    }
  }
  const bool pageLandscape = pageAspect > 1.0F;
  bool landscape = pageLandscape;
  switch (m_options.orientation) {
  case Orientation::Auto:
    landscape = (n == 2 || n == 6) ? !pageLandscape : pageLandscape;
    break;
  case Orientation::Portrait:
    landscape = false;
    break;
  case Orientation::Landscape:
    landscape = true;
    break;
  }
  float paperAspect = pageLandscape ? 1.0F / pageAspect : pageAspect; // short side / long side
  if (m_target == Target::Printer)
    if (const auto mm = mediaSizeMm(m_options.media); mm && mm->first > 0 && mm->second > 0)
      paperAspect = static_cast<float>(std::min(mm->first, mm->second) / std::max(mm->first, mm->second));
  const float aspect = landscape ? 1.0F / paperAspect : paperAspect;
  const Grid g = gridFor(n, landscape);
  const float s = height / (size == Size::Page ? kPageHeight : kThumbHeight);
  return std::make_unique<SheetNode>(std::move(cells), g.cols, g.rows, height * aspect, height, s, selected);
}

std::unique_ptr<Node> PrintSheet::buildPreview(Renderer& renderer, float s, float width) {
  const float caption = Style::fontSizeCaption * s;
  auto pv = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP12 * s,
                        .padding = Tokens::spaceP16 * s, .fill = colorSpecFromRole(ColorRole::Fill1),
                        .radius = Tokens::radiusR16 * s, .flexGrow = 1.0F});
  const auto note = [&](std::string text) {
    return ui::label({.text = std::move(text), .fontSize = caption, .fontWeight = FontWeight::Medium,
                      .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2,
                      .ellipsize = TextEllipsize::End});
  };
  switch (m_preview) {
  case Preview::Loading:
    pv->addChild(note("Preparing preview…"));
    return pv;
  case Preview::Failed:
    pv->addChild(ui::glyph({.glyph = std::string("file"), .glyphSize = 40.0F * s,
                            .color = colorSpecFromRole(ColorRole::Label3)}));
    pv->addChild(note("No preview for this file. It prints as usual."));
    return pv;
  case Preview::Ready:
    break;
  }
  const std::vector<int> pages = pagesToPrint();
  const int n = perSheet();
  const int sheets = std::max(1, (static_cast<int>(pages.size()) + n - 1) / n);
  m_sheet = std::clamp(m_sheet, 0, sheets - 1);
  pv->addChild(buildSheet(renderer, m_sheet, pages, kPageHeight * s, Size::Page, false));

  if (sheets > 1) {
    const float gap = Tokens::spaceP8 * s;
    auto strip = ui::row({.align = FlexAlign::Center, .gap = gap});
    // As many thumbnails as the pane holds: sheet padding, column and gap, then the pane's own padding.
    const float pane = width - (2.0F * Tokens::spaceP12 + kColumnWidth + Tokens::spaceP16 + 2.0F * Tokens::spaceP16) * s;
    const float thumbW = buildSheet(renderer, m_sheet, pages, kThumbHeight * s, Size::Thumb, false)->width() + gap;
    const int visible = std::clamp(static_cast<int>((pane + gap) / thumbW), 1, kThumbsVisible);
    const int first = std::clamp(m_sheet - visible / 2, 0, std::max(0, sheets - visible));
    for (int i = first; i < std::min(sheets, first + visible); ++i) {
      auto thumb = buildSheet(renderer, i, pages, kThumbHeight * s, Size::Thumb, i == m_sheet);
      const float w = thumb->width();
      const float h = thumb->height();
      strip->addChild(ui::inputArea({.width = w, .height = h,
                                     .onClick = [this, i](const InputArea::PointerData&) { showSheet(i); }},
                                    std::move(thumb)));
    }
    pv->addChild(std::move(strip));
  }
  const char* unit = n > 1 ? "Sheet" : "Page";
  pv->addChild(note(sheets > 1 ? std::format("{} {} of {} · ←/→", unit, m_sheet + 1, sheets)
                               : std::format("{} 1 of 1", unit)));
  return pv;
}

namespace {
  // The shared row builders of both screens.
  // Keys let AppWindow put keyboard focus back on the same control after a rebuild.
  void tagFocus(Node& node, const std::string& prefix, int& n) {
    if (auto* seg = dynamic_cast<Segmented*>(&node)) {
      if (InputArea* area = seg->focusArea())
        area->setTabFocusKey(std::format("{}.{}", prefix, n++));
      return;
    }
    if (auto* area = dynamic_cast<InputArea*>(&node); area && area->focusable() && area->tabStop())
      area->setTabFocusKey(std::format("{}.{}", prefix, n++));
    for (const auto& child : node.children())
      tagFocus(*child, prefix, n);
  }

  void tagFocus(Node& node, const std::string& prefix) {
    int n = 0;
    tagFocus(node, prefix, n);
  }

  struct Rows {
    float s;
    float body;
    float caption;
    float rowH;
    float ctlH;

    std::unique_ptr<Flex> card() const {
      return ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                         .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Tokens::radiusR16 * s,
                         .border = colorSpecFromRole(ColorRole::GlassCardEdge),
                         .borderWidth = std::max(1.0F, 0.5F * s), .fillWidth = true});
    }

    std::unique_ptr<Label> label(std::string text, ColorRole role = ColorRole::Label1, float size = 0.0F) const {
      return ui::label({.text = std::move(text), .fontSize = size > 0.0F ? size : body,
                        .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(role), .maxLines = 2,
                        .ellipsize = TextEllipsize::End});
    }

    // A row with its key; rows after the first get a hairline on top (mockup .opt + .opt).
    void opt(Flex& card, std::string key, std::unique_ptr<Node> value) const {
      if (card.children().size() > 0)
        card.addChild(ui::row({.fill = colorSpecFromRole(ColorRole::Separator), .fillWidth = true,
                               .height = std::max(1.0F, 0.5F * s)}));
      auto r = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP12 * s,
                        .height = rowH});
      auto keyBox = ui::row({.align = FlexAlign::Center, .width = kKeyWidth * s});
      keyBox->addChild(ui::label({.text = key, .fontSize = body, .fontWeight = FontWeight::Medium,
                                  .color = colorSpecFromRole(ColorRole::Label2), .maxWidth = kKeyWidth * s,
                                  .maxLines = 1, .ellipsize = TextEllipsize::End}));
      r->addChild(std::move(keyBox));
      tagFocus(*value, "print." + key);
      value->setFlexGrow(1.0F);
      r->addChild(std::move(value));
      card.addChild(std::move(r));
    }

    std::unique_ptr<Flex> note(std::string text, ColorRole role, Label** out = nullptr) const {
      auto r = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP12 * s});
      r->addChild(ui::row({.width = (kKeyWidth + Tokens::spaceP12) * s}));
      auto l = label(std::move(text), role, caption);
      l->setFlexGrow(1.0F);
      if (out)
        *out = l.get();
      r->addChild(std::move(l));
      return r;
    }

    std::unique_ptr<Flex> seg(std::vector<std::string> labels, std::size_t on,
                              std::function<void(std::size_t)> change) const {
      std::vector<ui::SegmentedOption> options;
      for (auto& l : labels)
        options.push_back({.label = std::move(l)});
      auto wrap = ui::row({.align = FlexAlign::Center});
      wrap->addChild(ui::segmented({.options = std::move(options), .selectedIndex = on, .fontSize = body,
                                    .onChange = std::move(change)}));
      return wrap;
    }

    std::unique_ptr<Flex> select(std::vector<std::string> options, std::size_t on,
                                 std::function<void(std::size_t)> change) const {
      auto wrap = ui::row({.align = FlexAlign::Center});
      wrap->addChild(ui::select({.options = std::move(options), .selectedIndex = on, .fontSize = body,
                                 .controlHeight = ctlH, .glyphSize = 14.0F * s,
                                 .onSelectionChanged = [change = std::move(change)](std::size_t i, std::string_view) {
                                   change(i);
                                 }}));
      return wrap;
    }

    std::unique_ptr<Flex> toggle(bool on, std::function<void(bool)> change) const {
      auto wrap = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End});
      wrap->addChild(ui::toggle({.checked = on, .onChange = std::move(change)}));
      return wrap;
    }
  };
} // namespace

void PrintSheet::buildMain(Node& col, float s) {
  const Rows rows{s, Style::fontSizeBody * s, Style::fontSizeCaption * s, Tokens::sizeRow * s,
                  Tokens::sizeControlM * s};
  const auto rebuild = [this] {
    if (m_cb.rebuild)
      m_cb.rebuild();
  };

  auto ttl = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s});
  ttl->addChild(ui::label({.text = std::format("Print “{}”", m_request.title.empty() ? "Document" : m_request.title),
                           .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                           .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                           .ellipsize = TextEllipsize::Middle}));
  ttl->addChild(rows.label(m_pageCount > 0 ? std::format("{} · {} {}", appName(m_request.appId), m_pageCount,
                                                         m_pageCount == 1 ? "page" : "pages")
                                           : appName(m_request.appId),
                           ColorRole::Label2, rows.caption));
  col.addChild(std::move(ttl));

  auto card = rows.card();
  if (m_printersState == Printers::Loading) {
    rows.opt(*card, "Printer", rows.label("Looking for printers…", ColorRole::Label2));
    col.addChild(std::move(card));
    return;
  }

  std::vector<std::string> names;
  std::vector<std::string> states;
  std::vector<ColorSpec> dots;
  for (const PrinterInfo& each : m_printers) {
    names.push_back(each.label);
    states.push_back(stateText(each));
    dots.push_back(stateColour(each.state));
  }
  names.emplace_back("Save as PDF");
  states.emplace_back();
  auto printerSelect = ui::row({.align = FlexAlign::Center, .flexGrow = 1.0F});
  printerSelect->addChild(ui::select(
      {.options = std::move(names),
       .selectedIndex = m_target == Target::SaveAsPdf ? m_printers.size() : m_printer, .fontSize = rows.body,
       .controlHeight = rows.ctlH, .glyphSize = 14.0F * s, .optionIndicators = std::move(dots), .optionSubtitles = std::move(states),
       .separatorBefore = m_printers.size(), .actions = {"Add Printer…"},
       .onAction = [](std::size_t) { openPrinterSettings(); }, .flexGrow = 1.0F,
       .onSelectionChanged = [this, rebuild](std::size_t i, std::string_view) {
         if (i < m_printers.size())
           selectPrinter(i);
         else
           selectSaveAsPdf();
         rebuild();
       }}));
  rows.opt(*card, "Printer", std::move(printerSelect));

  const PrinterInfo* p = printer();
  if (!p) {
    card->addChild(rows.note(m_printers.empty() ? "No printers found. Add one in Settings › Printers, or save a PDF."
                                                : "Saves the document as the app made it.",
                             ColorRole::Label2));
    col.addChild(std::move(card));
    return;
  }

  auto st = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s});
  st->addChild(ui::row({.width = (kKeyWidth + Tokens::spaceP12 - Tokens::spaceP8) * s}));
  const float dot = 8.0F * s;
  st->addChild(ui::row({.fill = stateColour(p->state), .radius = dot / 2.0F, .width = dot, .height = dot}));
  auto stl = rows.label(stateText(*p), p->state == PrinterState::NeedsAttention ? ColorRole::DangerText
                                                                                 : ColorRole::Label2,
                        rows.caption);
  stl->setFlexGrow(1.0F);
  st->addChild(std::move(stl));
  card->addChild(std::move(st));

  auto copies = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  copies->addChild(ui::stepper({.minValue = 1, .maxValue = 999, .step = 1, .value = m_options.copies, .scale = s,
                                .onValueChanged = [this](int v) { m_options.copies = v; }}));
  copies->addChild(ui::row({.flexGrow = 1.0F}));
  copies->addChild(rows.label("Collate"));
  copies->addChild(ui::toggle({.checked = m_options.collate, .onChange = [this](bool on) { m_options.collate = on; }}));
  rows.opt(*card, "Copies", std::move(copies));

  auto pages = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  pages->addChild(rows.seg({m_pageCount > 0 ? std::format("All {}", m_pageCount) : std::string("All"), "Range"},
                           m_allPages ? 0 : 1, [this, rebuild](std::size_t i) {
                             m_allPages = i == 0;
                             rebuild();
                           }));
  pages->addChild(ui::input({.out = &m_rangeInput, .value = m_rangeText, .placeholder = std::string("e.g. 1-2, 4"), .fontSize = rows.body,
                             .controlHeight = rows.ctlH, .invalid = !m_allPages && !m_rangeError.empty(),
                             .enabled = !m_allPages, .flexGrow = 1.0F,
                             .onChange = [this](const std::string& text) {
                               m_rangeText = text;
                               validateRange();
                               showRange();
                             },
                             .onSubmit = [this](const std::string&) { submit(); },
                             // The preview follows the range once the field is left.
                             .onFocusLoss =
                                 [rebuild] {
                                   if (!InputDispatcher::inSceneTeardown())
                                     rebuild();
                                 }}));
  rows.opt(*card, "Pages", std::move(pages));
  m_rangeNote = static_cast<Flex*>(card->addChild(rows.note(m_rangeError, ColorRole::DangerText, &m_rangeNoteLabel)));

  if (p->duplex)
    rows.opt(*card, "Two-sided",
             rows.seg({"Off", "Long Edge", "Short Edge"}, static_cast<std::size_t>(m_options.duplex),
                      [this](std::size_t i) { m_options.duplex = static_cast<Duplex>(i); }));
  if (p->colour)
    rows.opt(*card, "Colour", rows.seg({"Colour", "Black and White"}, m_options.colour ? 0 : 1,
                                       [this](std::size_t i) { m_options.colour = i == 0; }));
  if (!p->media.empty()) {
    std::vector<std::string> media;
    std::size_t on = 0;
    for (std::size_t i = 0; i < p->media.size(); ++i) {
      media.push_back(mediaLabel(p->media[i]));
      if (p->media[i] == m_options.media)
        on = i;
    }
    rows.opt(*card, "Paper", rows.select(std::move(media), on, [this, rebuild, all = p->media](std::size_t i) {
      m_options.media = all[i];
      rebuild();
    }));
    if (!m_appMedia.empty() && m_options.media != m_appMedia)
      card->addChild(rows.note(std::format("The app laid out the pages for {}. They print scaled to fit.",
                                           mediaLabel(m_appMedia)),
                               ColorRole::Label2));
  }
  col.addChild(std::move(card));

  auto more = rows.card();
  const float rowW = (kColumnWidth - 2.0F * Tokens::spaceP4) * s;
  auto moreRow = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP12 * s, .width = rowW,
                          .height = rows.rowH});
  auto moreLabel = rows.label("More Options…", ColorRole::AccentText);
  moreLabel->setFlexGrow(1.0F);
  moreRow->addChild(std::move(moreLabel));
  moreRow->addChild(ui::glyph({.glyph = std::string("chevron-right"), .glyphSize = 14.0F * s,
                               .color = colorSpecFromRole(ColorRole::AccentText)}));
  const auto openMore = [this, rebuild] {
    m_screen = Screen::More;
    rebuild();
  };
  more->addChild(ui::inputArea({.focusable = true, .width = rowW, .height = rows.rowH,
                                .onClick = [openMore](const InputArea::PointerData&) { openMore(); },
                                .configure =
                                    [openMore](InputArea& a) {
                                      a.setTabFocusKey("print.more");
                                      a.setOnKeyDown([openMore](const InputArea::KeyData& k) {
                                        if (KeybindMatcher::matches(KeybindAction::Validate, k.sym, k.modifiers))
                                          openMore();
                                      });
                                    }},
                               std::move(moreRow)));
  col.addChild(std::move(more));
}

void PrintSheet::buildMore(Node& col, float s) {
  const Rows rows{s, Style::fontSizeBody * s, Style::fontSizeCaption * s, Tokens::sizeRow * s,
                  Tokens::sizeControlM * s};
  const auto rebuild = [this] {
    if (m_cb.rebuild)
      m_cb.rebuild();
  };

  auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP4 * s});
  header->addChild(ui::button({.glyph = std::string("chevron-left"), .glyphSize = 14.0F * s,
                               .variant = ButtonVariant::Default, .tooltip = std::string("Back"),
                               .radius = 16.0F * s, .width = 32.0F * s, .height = 32.0F * s,
                               .onClick = [this, rebuild] {
                                 m_screen = Screen::Main;
                                 rebuild();
                               }}));
  header->addChild(ui::label({.text = std::string("More Options"), .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
  tagFocus(*header, "print.back");
  col.addChild(std::move(header));

  auto card = rows.card();
  static constexpr int kPerSheet[] = {1, 2, 4, 6};
  std::size_t up = 0;
  for (std::size_t i = 0; i < std::size(kPerSheet); ++i)
    if (kPerSheet[i] == m_options.numberUp)
      up = i;
  rows.opt(*card, "Per sheet", rows.seg({"1", "2", "4", "6"}, up, [this, rebuild](std::size_t i) {
    m_options.numberUp = kPerSheet[i];
    m_sheet = 0;
    rebuild();
  }));
  rows.opt(*card, "Orientation",
           rows.seg({"Auto", "Portrait", "Landscape"}, static_cast<std::size_t>(m_options.orientation),
                    [this, rebuild](std::size_t i) {
                      m_options.orientation = static_cast<Orientation>(i);
                      rebuild();
                    }));
  auto scale = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
  scale->addChild(rows.seg({"Fit", "100 %", "Custom"}, static_cast<std::size_t>(m_options.scaling),
                           [this, rebuild](std::size_t i) {
                             m_options.scaling = static_cast<Scaling>(i);
                             rebuild();
                           }));
  if (m_options.scaling == Scaling::Custom)
    scale->addChild(ui::stepper({.minValue = 10, .maxValue = 400, .step = 5, .value = m_options.scalePercent, .scale = s,
                                 .onValueChanged = [this](int v) { m_options.scalePercent = v; }}));
  rows.opt(*card, "Scale", std::move(scale));
  rows.opt(*card, "Margins", rows.select({"Default", "None", "Narrow", "Wide"},
                                         static_cast<std::size_t>(m_options.margins),
                                         [this](std::size_t i) { m_options.margins = static_cast<Margins>(i); }));
  rows.opt(*card, "Order", rows.select({"Front to Back", "Back to Front"}, m_options.reverseOrder ? 1 : 0,
                                       [this](std::size_t i) { m_options.reverseOrder = i == 1; }));
  rows.opt(*card, "Booklet", rows.toggle(m_options.booklet, [this](bool on) { m_options.booklet = on; }));
  col.addChild(std::move(card));

  const PrinterInfo* p = printer();
  if (!p || p->options.empty())
    return;
  auto caption = ui::row({.paddingV = Tokens::spaceP4 * s, .paddingH = Tokens::spaceP12 * s});
  caption->addChild(ui::label({.text = p->label, .fontSize = rows.caption, .fontWeight = FontWeight::SemiBold,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1,
                               .ellipsize = TextEllipsize::End}));
  col.addChild(std::move(caption));
  auto own = rows.card();
  for (const PrinterOption& o : p->options) {
    std::size_t on = o.defaultIndex;
    for (const auto& [k, v] : m_options.printerOptions)
      if (k == o.key)
        if (const auto it = std::ranges::find(o.values, v); it != o.values.end())
          on = static_cast<std::size_t>(it - o.values.begin());
    rows.opt(*own, o.label, rows.select(o.valueLabels, on, [this, key = o.key, values = o.values](std::size_t i) {
      auto& opts = m_options.printerOptions;
      std::erase_if(opts, [&](const auto& kv) { return kv.first == key; });
      opts.emplace_back(key, values[i]);
    }));
  }
  col.addChild(std::move(own));
}

std::unique_ptr<Node> PrintSheet::build(Renderer& renderer, float s, float width) {
  for (TextureHandle& texture : m_dropped)
    renderer.textureManager().unload(texture);
  m_dropped.clear();
  m_rangeInput = nullptr;
  m_rangeNote = nullptr;
  m_rangeNoteLabel = nullptr;
  m_printButton = nullptr;
  const float body = Style::fontSizeBody * s;
  const float ctlH = Tokens::sizeControlM * s;
  auto sheet = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP12 * s,
                        .flexGrow = 1.0F});
  sheet->addChild(buildPreview(renderer, s, width));

  auto col = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .width = kColumnWidth * s});
  if (m_screen == Screen::More)
    buildMore(*col, s);
  else
    buildMain(*col, s);
  col->addChild(ui::row({.flexGrow = 1.0F}));

  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP4 * s});
  Button::ButtonPalette plain = Button::defaultPalette(ButtonVariant::Default);
  plain.borderWidth = 0.0F;
  const bool pdf = m_target == Target::SaveAsPdf;
  if (!pdf && m_printersState == Printers::Loaded)
    buttons->addChild(ui::button({.text = std::string("Save as PDF…"), .fontSize = body,
                                  .variant = ButtonVariant::Default, .customPalette = plain,
                                  .paddingH = Tokens::spaceP12 * s, .radius = Tokens::radiusR8 * s, .height = ctlH,
                                  .onClick = [this] {
                                    if (m_cb.savePdf)
                                      m_cb.savePdf();
                                  }}));
  buttons->addChild(ui::row({.flexGrow = 1.0F}));
  buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = body, .variant = ButtonVariant::Default,
                                .customPalette = plain, .paddingH = Tokens::spaceP12 * s,
                                .radius = Tokens::radiusR8 * s, .height = ctlH, .onClick = [this] {
                                  if (m_cb.cancel)
                                    m_cb.cancel();
                                }}));
  buttons->addChild(ui::button({.out = &m_printButton, .text = std::string(pdf ? "Save…" : "Print"), .fontSize = body,
                                .enabled = !m_sent && (pdf || (printer() != nullptr && rangeValid())),
                                .variant = ButtonVariant::Primary, .paddingH = Tokens::spaceP12 * s,
                                .radius = Tokens::radiusR8 * s, .height = ctlH, .onClick = [this] { submit(); }}));
  tagFocus(*buttons, "print.buttons");
  col->addChild(std::move(buttons));
  sheet->addChild(std::move(col));
  showRange();
  return sheet;
}
