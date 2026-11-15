#include "print_sheet.h"

#include "app/app_loop.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "design_tokens.h"
#include "helper_client.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/core/texture_manager.h"
#include "ui/builders.h"
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

  // The rendered page; uploads new pixels on the next layout, where a Renderer is at hand.
  class PageNode final : public Node {
  public:
    PageNode(std::shared_ptr<PrintSheet::Pixels> pixels, float height) : m_pixels(std::move(pixels)) {
      m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
      m_image->setFit(ImageFit::Contain);
      const float aspect = m_pixels && m_pixels->height > 0
          ? static_cast<float>(m_pixels->width) / static_cast<float>(m_pixels->height)
          : 0.773F; // A4 until the first page arrives
      m_width = height * aspect;
      m_height = height;
      setSize(m_width, m_height);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_pixels && !m_loaded) {
        m_image->setSourceRaw(renderer, m_pixels->bgra.data(), m_pixels->bgra.size(), m_pixels->width,
                              m_pixels->height, m_pixels->stride, PixmapFormat::BGRA);
        m_loaded = true;
      }
      m_image->setSize(m_width, m_height);
      m_image->layout(renderer);
    }

  private:
    std::shared_ptr<PrintSheet::Pixels> m_pixels;
    Image* m_image = nullptr;
    float m_width = 0.0F;
    float m_height = 0.0F;
    bool m_loaded = false;
  };

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
      renderPage(0);
    }
    if (m_cb.rebuild)
      m_cb.rebuild();
  });
}

void PrintSheet::renderPage(int page) {
  if (!m_helper || page < 0 || page >= m_pageCount)
    return;
  m_page = page;
  const std::uint64_t generation = ++m_renderGeneration;
  m_helper->request({.op = preview::op::Render, .a = static_cast<std::uint32_t>(page), .b = kPageRenderWidth,
                     .c = kPageRenderWidth * 2},
                    [this, generation, alive = std::weak_ptr<bool>(m_alive)](const preview::HelperReply& r) {
                      if (alive.expired() || generation != m_renderGeneration)
                        return;
                      if (r.msg.op != preview::op::Page || !r.data) {
                        m_preview = Preview::Failed;
                      } else {
                        auto px = std::make_shared<Pixels>();
                        const auto bytes = r.data->bytes();
                        px->bgra.assign(bytes.begin(), bytes.end());
                        px->width = static_cast<int>(r.msg.b);
                        px->height = static_cast<int>(r.msg.c);
                        px->stride = static_cast<int>(r.msg.d);
                        m_pagePixels = std::move(px);
                        m_preview = Preview::Ready;
                      }
                      if (m_cb.rebuild)
                        m_cb.rebuild();
                    });
}

const PrinterInfo* PrintSheet::printer() const {
  return m_printer < m_printers.size() ? &m_printers[m_printer] : nullptr;
}

void PrintSheet::selectPrinter(std::size_t index) {
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

bool PrintSheet::rangeValid() const { return m_allPages || (m_rangeError.empty() && !m_rangeText.empty()); }

void PrintSheet::submit() {
  if (m_sent || !printer() || !rangeValid())
    return;
  if (!m_allPages)
    m_options.ranges = parsePageRanges(m_rangeText, std::max(m_pageCount, 1)).first;
  m_sent = true;
  if (m_cb.print)
    m_cb.print(m_options);
}

bool PrintSheet::firstKey(const KeyboardEvent& event) {
  if (!event.pressed || event.modifiers != 0)
    return false;
  switch (event.sym) {
  case XKB_KEY_Escape:
    if (m_cb.cancel)
      m_cb.cancel();
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    submit();
    return true;
  default:
    return false;
  }
}

bool PrintSheet::key(const KeyboardEvent& event) {
  if (!event.pressed || m_pageCount < 2 || (event.sym != XKB_KEY_Left && event.sym != XKB_KEY_Right))
    return false;
  renderPage(std::clamp(m_page + (event.sym == XKB_KEY_Left ? -1 : 1), 0, m_pageCount - 1));
  return true;
}

std::unique_ptr<Node> PrintSheet::build(float s) {
  const float rowH = Tokens::sizeRow * s;
  const float ctlH = Tokens::sizeControlM * s;
  const float body = Style::fontSizeBody * s;
  const float caption = Style::fontSizeCaption * s;
  const auto label = [&](std::string text, ColorRole role = ColorRole::Label1, float size = 0.0F) {
    return ui::label({.text = std::move(text), .fontSize = size > 0.0F ? size : body, .fontWeight = FontWeight::Medium,
                      .color = colorSpecFromRole(role), .maxLines = 2, .ellipsize = TextEllipsize::End});
  };

  auto sheet = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * s, .padding = Tokens::spaceP12 * s,
                        .flexGrow = 1.0F});

  // Left: the preview.
  auto pv = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP12 * s,
                        .padding = Tokens::spaceP16 * s, .fill = colorSpecFromRole(ColorRole::Fill1),
                        .radius = Tokens::radiusR12 * s, .flexGrow = 1.0F});
  switch (m_preview) {
  case Preview::Loading:
    pv->addChild(label("Preparing preview…", ColorRole::Label2, caption));
    break;
  case Preview::Failed:
    pv->addChild(ui::glyph({.glyph = std::string("file"), .glyphSize = 40.0F * s, .color = colorSpecFromRole(ColorRole::Label3)}));
    pv->addChild(label("No preview for this file. It prints as usual.", ColorRole::Label2, caption));
    break;
  case Preview::Ready:
    pv->addChild(std::make_unique<PageNode>(m_pagePixels, kPageHeight * s));
    pv->addChild(label(m_pageCount > 1 ? std::format("Page {} of {} · ←/→", m_page + 1, m_pageCount)
                                       : std::string("Page 1 of 1"),
                       ColorRole::Label2, caption));
    break;
  }
  sheet->addChild(std::move(pv));

  // Right: title, the choices card, buttons.
  auto col = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .width = kColumnWidth * s});
  auto ttl = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s});
  ttl->addChild(ui::label({.text = std::format("Print “{}”", m_request.title.empty() ? "Document" : m_request.title),
                           .fontSize = Tokens::typeTitle3.size * s, .fontWeight = FontWeight::SemiBold,
                           .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                           .ellipsize = TextEllipsize::Middle}));
  ttl->addChild(label(m_pageCount > 0 ? std::format("{} · {} {}", appName(m_request.appId), m_pageCount,
                                                    m_pageCount == 1 ? "page" : "pages")
                                      : appName(m_request.appId),
                      ColorRole::Label2, caption));
  col->addChild(std::move(ttl));

  auto card = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP4 * s,
                          .fill = colorSpecFromRole(ColorRole::GlassCard), .radius = Tokens::radiusR12 * s,
                          .border = colorSpecFromRole(ColorRole::GlassCardEdge), .borderWidth = std::max(1.0F, 0.5F * s),
                          .fillWidth = true});
  const auto opt = [&](std::string key, std::unique_ptr<Node> value) {
    auto r = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingH = Tokens::spaceP12 * s,
                      .height = rowH});
    r->addChild(ui::label({.text = std::move(key), .fontSize = body, .fontWeight = FontWeight::Medium,
                           .color = colorSpecFromRole(ColorRole::Label2), .width = kKeyWidth * s, .maxLines = 1,
                           .ellipsize = TextEllipsize::End}));
    value->setFlexGrow(1.0F);
    r->addChild(std::move(value));
    return r;
  };
  const auto note = [&](std::string text, ColorRole role) {
    auto r = ui::row({.align = FlexAlign::Center, .paddingH = Tokens::spaceP12 * s});
    r->addChild(ui::row({.width = (kKeyWidth + Tokens::spaceP12) * s}));
    auto l = label(std::move(text), role, caption);
    l->setFlexGrow(1.0F);
    r->addChild(std::move(l));
    return r;
  };
  const auto seg = [&](std::vector<std::string> labels, std::size_t on, std::function<void(std::size_t)> change) {
    std::vector<ui::SegmentedOption> options;
    for (auto& l : labels)
      options.push_back({.label = std::move(l)});
    auto wrap = ui::row({.align = FlexAlign::Center});
    wrap->addChild(ui::segmented({.options = std::move(options), .selectedIndex = on, .fontSize = body,
                                  .onChange = std::move(change)}));
    return wrap;
  };
  const auto rebuild = [this] {
    if (m_cb.rebuild)
      m_cb.rebuild();
  };

  const PrinterInfo* p = printer();
  if (m_printersState == Printers::Loading) {
    card->addChild(opt("Printer", label("Looking for printers…", ColorRole::Label2)));
  } else if (!p) {
    card->addChild(opt("Printer", label("No printers found", ColorRole::Label2)));
    card->addChild(note("Add a printer in Settings › Printers.", ColorRole::Label2));
  } else {
    std::vector<std::string> names;
    std::vector<ColorSpec> dots;
    for (const PrinterInfo& each : m_printers) {
      names.push_back(each.label);
      dots.push_back(stateColour(each.state));
    }
    card->addChild(opt("Printer", ui::select({.options = std::move(names), .selectedIndex = m_printer,
                                              .fontSize = body, .controlHeight = ctlH,
                                              .optionIndicators = std::move(dots),
                                              .onSelectionChanged = [this, rebuild](std::size_t i, std::string_view) {
                                                selectPrinter(i);
                                                rebuild();
                                              }})));
    auto st = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s, .paddingH = Tokens::spaceP12 * s});
    st->addChild(ui::row({.width = (kKeyWidth + Tokens::spaceP12 - Tokens::spaceP8) * s}));
    const float dot = 8.0F * s;
    st->addChild(ui::row({.fill = stateColour(p->state), .radius = dot / 2.0F, .width = dot, .height = dot}));
    auto stl = label(stateText(*p), p->state == PrinterState::NeedsAttention ? ColorRole::DangerText : ColorRole::Label2,
                     caption);
    stl->setFlexGrow(1.0F);
    st->addChild(std::move(stl));
    card->addChild(std::move(st));

    auto copies = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    copies->addChild(ui::stepper({.minValue = 1, .maxValue = 999, .step = 1, .value = m_options.copies,
                                  .onValueChanged = [this](int v) { m_options.copies = v; }}));
    copies->addChild(ui::row({.flexGrow = 1.0F}));
    copies->addChild(label("Collate"));
    copies->addChild(ui::toggle({.checked = m_options.collate, .onChange = [this](bool on) { m_options.collate = on; }}));
    card->addChild(opt("Copies", std::move(copies)));

    auto pages = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    pages->addChild(seg({m_pageCount > 0 ? std::format("All {}", m_pageCount) : std::string("All"), "Range"},
                        m_allPages ? 0 : 1, [this, rebuild](std::size_t i) {
                          m_allPages = i == 0;
                          rebuild();
                        }));
    pages->addChild(ui::input({.value = m_rangeText, .placeholder = std::string("e.g. 1-2, 4"), .fontSize = body,
                               .controlHeight = ctlH, .invalid = !m_allPages && !m_rangeError.empty(),
                               .enabled = !m_allPages, .flexGrow = 1.0F,
                               .onChange = [this, rebuild](const std::string& text) {
                                 const bool had = !m_rangeError.empty();
                                 m_rangeText = text;
                                 m_rangeError = text.empty() ? std::string()
                                                             : parsePageRanges(text, std::max(m_pageCount, 1))
                                                                   .second.value_or(RangeError{})
                                                                   .message;
                                 if (had != !m_rangeError.empty())
                                   rebuild();
                               },
                               .onSubmit = [this](const std::string&) { submit(); }}));
    card->addChild(opt("Pages", std::move(pages)));
    if (!m_allPages && !m_rangeError.empty())
      card->addChild(note(m_rangeError, ColorRole::DangerText));

    if (p->duplex)
      card->addChild(opt("Two-sided", seg({"Off", "Long Edge", "Short Edge"}, static_cast<std::size_t>(m_options.duplex),
                                          [this](std::size_t i) { m_options.duplex = static_cast<Duplex>(i); })));
    if (p->colour)
      card->addChild(opt("Colour", seg({"Colour", "Black and White"}, m_options.colour ? 0 : 1,
                                       [this](std::size_t i) { m_options.colour = i == 0; })));
    if (!p->media.empty()) {
      std::vector<std::string> media;
      std::size_t on = 0;
      for (std::size_t i = 0; i < p->media.size(); ++i) {
        media.push_back(mediaLabel(p->media[i]));
        if (p->media[i] == m_options.media)
          on = i;
      }
      auto paper = ui::row({.align = FlexAlign::Center});
      paper->addChild(ui::select({.options = std::move(media), .selectedIndex = on, .fontSize = body,
                                  .controlHeight = ctlH,
                                  .onSelectionChanged = [this, rebuild, all = p->media](std::size_t i, std::string_view) {
                                    m_options.media = all[i];
                                    rebuild();
                                  }}));
      card->addChild(opt("Paper", std::move(paper)));
      if (!m_appMedia.empty() && m_options.media != m_appMedia)
        card->addChild(note(std::format("The app laid out the pages for {}. They print scaled to fit.",
                                        mediaLabel(m_appMedia)),
                            ColorRole::Label2));
    }
  }
  col->addChild(std::move(card));
  col->addChild(ui::row({.flexGrow = 1.0F}));

  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                          .padding = Tokens::spaceP4 * s});
  Button::ButtonPalette plain = Button::defaultPalette(ButtonVariant::Default);
  plain.borderWidth = 0.0F;
  buttons->addChild(ui::button({.text = std::string("Cancel"), .fontSize = body, .variant = ButtonVariant::Default,
                                .customPalette = plain, .paddingH = Tokens::spaceP12 * s,
                                .radius = Tokens::radiusR8 * s, .height = ctlH, .onClick = [this] {
                                  if (m_cb.cancel)
                                    m_cb.cancel();
                                }}));
  buttons->addChild(ui::button({.text = std::string("Print"), .fontSize = body,
                                .enabled = p != nullptr && rangeValid() && !m_sent, .variant = ButtonVariant::Primary,
                                .paddingH = Tokens::spaceP12 * s, .radius = Tokens::radiusR8 * s, .height = ctlH,
                                .onClick = [this] { submit(); }}));
  col->addChild(std::move(buttons));
  sheet->addChild(std::move(col));
  return sheet;
}
