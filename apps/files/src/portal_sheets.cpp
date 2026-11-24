#include "portal_sheets.h"

#include "app_chooser_sheet.h"
#include "design_tokens.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <format>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

namespace {
  class PictureNode final : public Node {
  public:
    PictureNode(std::string path, float width, float height, float radius)
        : m_path(std::move(path)), m_width(width), m_height(height) {
      m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
      m_image->setFit(ImageFit::Cover);
      m_image->setRadius(radius < 0.0F ? std::min(width, height) / 2.0F : radius);
      m_image->setSize(width, height);
      setSize(width, height);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const int px = static_cast<int>(std::ceil(std::max(m_width, m_height) * renderer.renderScale()));
      if (px != m_loadedPx && !m_path.empty()) {
        m_image->setSourceFile(renderer, m_path, px);
        m_loadedPx = px;
      }
      m_image->setSize(m_width, m_height);
      m_image->layout(renderer);
    }

  private:
    std::string m_path;
    float m_width;
    float m_height;
    Image* m_image = nullptr;
    int m_loadedPx = 0;
  };

  std::string appName(const std::string& appId) {
    if (appId.empty()) {
      return i18n::tr("files.portal.an-app");
    }
    std::string name = appId;
    if (GDesktopAppInfo* info = g_desktop_app_info_new((appId + ".desktop").c_str())) {
      name = g_app_info_get_name(G_APP_INFO(info));
      g_object_unref(info);
    }
    return name;
  }
} // namespace

std::unique_ptr<Node> pictureNode(std::string path, float width, float height, float radius) {
  if (path.empty()) {
    return ui::column({.fill = colorSpecFromRole(ColorRole::Fill2),
                       .radius = radius < 0.0F ? std::min(width, height) / 2.0F : radius, .width = width,
                       .height = height});
  }
  return std::make_unique<PictureNode>(std::move(path), width, height, radius);
}

PortalSheet::PortalSheet(SystemPortals::Request request, std::function<void(std::uint32_t, const std::string&)> answer)
    : m_request(std::move(request)), m_answer(std::move(answer)) {
  if (m_request.kind == SystemPortals::Kind::Account) {
    m_user = SystemPortals::currentUser();
  }
  m_name = m_request.name;
}

std::string PortalSheet::titleFor(const SystemPortals::Request& request) {
  switch (request.kind) {
  case SystemPortals::Kind::Account:
    return i18n::tr("files.portal.title-account");
  case SystemPortals::Kind::Wallpaper:
    return i18n::tr("files.portal.title-wallpaper");
  case SystemPortals::Kind::Launcher:
    return i18n::tr(request.webApp ? "files.portal.title-web" : "files.portal.title-launcher");
  }
  return {};
}

void PortalSheet::accept() {
  if (m_answered || (m_request.kind == SystemPortals::Kind::Launcher && m_name.empty())) {
    return;
  }
  m_answered = true;
  m_answer(0, m_name);
}

void PortalSheet::cancel() {
  if (!m_answered) {
    m_answered = true;
    m_answer(1, {});
  }
}

std::unique_ptr<Node> PortalSheet::build(float s) {
  using Kind = SystemPortals::Kind;
  const std::string app = appName(m_request.appId);
  auto box = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .padding = Tokens::spaceP20 * s,
                         .flexGrow = 1.0F});
  const auto headline = [&](std::string text) {
    box->addChild(ui::label({.text = std::move(text), .fontSize = Tokens::typeTitle3.size * s,
                             .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                             .maxLines = 3, .textAlign = TextAlign::Center}));
  };
  const auto caption = [&](std::string text, ColorRole role) {
    box->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * s,
                             .color = colorSpecFromRole(role), .maxLines = 6, .textAlign = TextAlign::Center}));
  };
  std::string action;
  switch (m_request.kind) {
  case Kind::Account: {
    box->addChild(pictureNode(m_user.image, Tokens::spaceP64 * s, Tokens::spaceP64 * s, -1.0F));
    headline(i18n::tr("files.portal.account", "app", app));
    if (!m_request.reason.empty()) {
      caption(m_request.reason, ColorRole::Label2);
    }
    auto card = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .padding = Tokens::spaceP12 * s,
                            .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Style::scaledRadius(Tokens::radiusR8, s),
                            .fillWidth = true});
    card->addChild(ui::label({.text = m_user.name, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::Medium,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
    card->addChild(ui::label({.text = m_user.id, .fontSize = Style::fontSizeCaption * s,
                              .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
    box->addChild(std::move(card));
    action = i18n::tr("files.portal.share");
    break;
  }
  case Kind::Wallpaper: {
    box->addChild(pictureNode(m_request.path, Tokens::spaceP64 * 5.0F * s, Tokens::spaceP64 * 2.75F * s,
                              Style::scaledRadius(Tokens::radiusR8, s)));
    headline(i18n::tr("files.portal.wallpaper", "app", app));
    caption(i18n::tr(m_request.setOn == "background" ? "files.portal.desktop" : "files.portal.desktop-lock"), ColorRole::Label2);
    action = i18n::tr("files.portal.title-wallpaper");
    break;
  }
  case Kind::Launcher: {
    box->addChild(pictureNode(iconFileFor(m_request.path), Tokens::spaceP64 * s, Tokens::spaceP64 * s,
                              Style::scaledRadius(Tokens::radiusR12, s)));
    headline(i18n::tr("files.portal.add", "name", m_name.empty() ? m_request.name : m_name));
    if (m_request.editableName) {
      box->addChild(ui::input({.value = m_name, .placeholder = i18n::tr("files.portal.name"), .fontSize = Style::fontSizeBody * s,
                               .controlHeight = Style::controlHeight * s, .width = Tokens::spaceP64 * 4.5F * s,
                               .onChange = [this](const std::string& v) { m_name = v; },
                               .onSubmit = [this](const std::string&) { accept(); }}));
    }
    caption(m_request.webApp && !m_request.target.empty() ? m_request.target : i18n::tr("files.portal.from", "app", app),
            ColorRole::Label2);
    action = i18n::tr("files.portal.add-button");
    break;
  }
  }
  box->addChild(ui::node({.flexGrow = 1.0F}));
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s,
                          .fillWidth = true});
  // System dialog footers: both buttons are capsules (rulebook 9.5).
  const float btnH = Tokens::sizeControlM * s;
  buttons->addChild(ui::button({.text = std::string(i18n::tr("common.actions.cancel")), .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Default, .paddingH = Tokens::spaceP16 * s, .radius = btnH / 2.0F,
                                .height = btnH, .onClick = [this] { cancel(); }}));
  buttons->addChild(ui::button({.text = action, .fontSize = Style::fontSizeBody * s,
                                .variant = ButtonVariant::Primary, .paddingH = Tokens::spaceP16 * s, .radius = btnH / 2.0F,
                                .height = btnH, .onClick = [this] { accept(); }}));
  box->addChild(std::move(buttons));
  return box;
}
