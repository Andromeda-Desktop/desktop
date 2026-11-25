#include "shell/settings/settings_content_privacy.h"

#include "auth/biometric_enrollment.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <variant>
#include <utility>
#include <string>

namespace settings {
  namespace {
    using namespace page_rows;

    constexpr float kFieldWidth = 240.0F;

    std::unique_ptr<Node> valueLabel(std::string text, float scale) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale,
                        .color = colorSpecFromRole(ColorRole::Label2)});
    }

    void addLinkRow(Flex& card, std::string_view glyph, std::string label, std::string value, std::string detail,
                    bool enabled, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->addChild(symbol(glyph, Style::fontSizeBody * scale, ColorRole::Label2));
      r->addChild(title(label, scale));
      r->addChild(valueLabel(std::move(value), scale));
      if (enabled) {
        r->addChild(iconButton("chevron-right", std::move(label), scale,
                               [open = ctx.openDetail, detail = std::move(detail)] { open(detail); }));
      }
      card.addChild(std::move(r));
    }

    void addToggleRow(Flex& card, std::string label, bool checked, std::vector<std::string> path,
                      const SettingsPrivacyContext& ctx) {
      auto r = row(ctx.scale);
      r->addChild(title(std::move(label), ctx.scale));
      r->addChild(ui::toggle({
          .checked = checked,
          .scale = ctx.scale,
          .onChange = [set = ctx.setBool, path = std::move(path)](bool on) { set(path, on); },
      }));
      card.addChild(std::move(r));
    }

    void addStatus(Flex& content, const SettingsPrivacyContext& ctx) {
      if (ctx.status.empty()) {
        return;
      }
      auto r = row(ctx.scale);
      r->addChild(ui::label({.text = ctx.status, .fontSize = Style::fontSizeCaption * ctx.scale,
                             .color = colorSpecFromRole(ColorRole::DangerText)}));
      content.addChild(std::move(r));
    }

    // The running enrollment: prompt, progress and Cancel, in place of the list.
    void addEnrollCard(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      r->addChild(title(ctx.enrollPrompt, scale));
      r->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Default, scale, ctx.cancelEnroll));
      card->addChild(std::move(r));
      auto bar = row(scale);
      bar->addChild(ui::progressBar({
          .fill = colorSpecFromRole(ColorRole::Accent),
          .track = colorSpecFromRole(ColorRole::Fill2),
          .progress = ctx.enrollProgress,
          .height = Style::sliderTrackHeight * scale,
          .flexGrow = 1.0F,
      }));
      card->addChild(std::move(bar));
    }

    void addFacePage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      if (!ctx.facePresent) {
        addEmptyState(content, "face-id-error", i18n::tr("settings.privacy.face-unavailable"), scale);
        return;
      }
      Flex& section = addHeader(content, i18n::tr("settings.privacy.faces"), nullptr, scale);
      if (ctx.enrolling == 1) {
        addEnrollCard(section, ctx);
      } else {
        Flex* card = addCard(section, scale);
        for (const auto& face : ctx.faces) {
          auto r = row(scale);
          r->setGap(Style::spaceMd * scale);
          r->addChild(symbol("face-id", Style::fontSizeBody * scale, ColorRole::Label2));
          r->addChild(title(face, scale));
          r->addChild(plainButton(i18n::tr("settings.privacy.rename"), ButtonVariant::Default, scale,
                                  [open = ctx.openDetail, face] { open("privacy:" + face + "\trename"); }));
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [remove = ctx.removeFace, face] { remove(face); }));
          card->addChild(std::move(r));
        }
        auto add = row(scale);
        add->addChild(plainButton(i18n::tr("settings.privacy.add-face"), ButtonVariant::Default, scale, ctx.addFace));
        card->addChild(std::move(add));
      }
      addStatus(content, ctx);

      Flex* use = addSection(content, i18n::tr("settings.privacy.face-use-for"), nullptr, scale);
      addToggleRow(*use, i18n::tr("settings.privacy.use-unlock"), ctx.faceUnlock, {"lockscreen", "face"}, ctx);
      if (ctx.faceAdminKnown) {
        auto r = row(scale);
        r->addChild(title(i18n::tr("settings.privacy.use-admin"), scale));
        r->addChild(ui::toggle({.checked = ctx.faceAdmin, .scale = scale, .onChange = ctx.setFaceAdmin}));
        use->addChild(std::move(r));
      }
      addToggleRow(*use, i18n::tr("settings.schema.lockscreen.face-on-request.label"), ctx.faceOnRequest,
                   {"lockscreen", "face_on_request"}, ctx);
    }

    void addFingerPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      if (!ctx.fingerPresent) {
        addEmptyState(content, "fingerprint-off",
                      i18n::tr(ctx.fprintdMissing ? "settings.privacy.finger-no-fprintd"
                                                  : "settings.privacy.finger-unavailable"),
                      scale);
        return;
      }
      Flex& section = addHeader(content, i18n::tr("settings.privacy.fingers"), nullptr, scale);
      if (ctx.enrolling == 2) {
        addEnrollCard(section, ctx);
      } else {
        Flex* card = addCard(section, scale);
        for (const auto& finger : ctx.fingers) {
          auto r = row(scale);
          r->setGap(Style::spaceMd * scale);
          r->addChild(symbol("fingerprint", Style::fontSizeBody * scale, ColorRole::Label2));
          r->addChild(title(BiometricEnrollment::fingerLabel(finger), scale));
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [remove = ctx.removeFinger, finger] { remove(finger); }));
          card->addChild(std::move(r));
        }
        auto add = row(scale);
        add->addChild(
            plainButton(i18n::tr("settings.privacy.add-finger"), ButtonVariant::Default, scale, ctx.addFinger)
        );
        card->addChild(std::move(add));
      }
      addStatus(content, ctx);

      Flex* use = addSection(content, i18n::tr("settings.privacy.finger-use-for"), nullptr, scale);
      addToggleRow(*use, i18n::tr("settings.privacy.use-unlock"), ctx.fingerprintUnlock, {"lockscreen", "fingerprint"},
                   ctx);
      if (ctx.fingerAdminKnown) {
        auto r = row(scale);
        r->addChild(title(i18n::tr("settings.privacy.use-admin"), scale));
        r->addChild(ui::toggle({.checked = ctx.fingerAdmin, .scale = scale, .onChange = ctx.setFingerAdmin}));
        use->addChild(std::move(r));
      }
    }

    void addRenamePage(Flex& content, const SettingsPrivacyContext& ctx, const std::string& from) {
      const float scale = ctx.scale;
      Flex* card = addCard(content, scale);
      auto draft = std::make_shared<std::string>(from);
      auto r = row(scale);
      r->addChild(title(i18n::tr("settings.privacy.face-name"), scale));
      auto submit = [rename = ctx.renameFace, from, draft] { rename(from, *draft); };
      r->addChild(ui::input({
          .value = from,
          .width = kFieldWidth * scale,
          .onChange = [draft](const std::string& text) { *draft = text; },
          .onSubmit = [submit](const std::string&) { submit(); },
      }));
      card->addChild(std::move(r));
      addStatus(content, ctx);
      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale,
                              .fillWidth = true});
      actions->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Default, scale, ctx.back));
      actions->addChild(plainButton(i18n::tr("settings.privacy.save"), ButtonVariant::Primary, scale, submit));
      content.addChild(std::move(actions));
    }

    std::unique_ptr<Node> secretInput(std::string* value, float scale, std::function<void()> submit) {
      return ui::input({
          .value = *value,
          .passwordMode = true,
          .width = kFieldWidth * scale,
          .onChange = [value](const std::string& text) { *value = text; },
          .onSubmit = [submit = std::move(submit)](const std::string&) { submit(); },
      });
    }

    void addPasswordPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      PasswordDraft* draft = ctx.password;
      Flex* card = addCard(content, scale);
      const std::pair<const char*, std::string*> fields[] = {
          {"settings.privacy.password-current", &draft->current},
          {"settings.privacy.password-new", &draft->next},
          {"settings.privacy.password-verify", &draft->verify},
      };
      for (const auto& [key, value] : fields) {
        auto r = row(scale);
        r->addChild(title(i18n::tr(key), scale));
        r->addChild(secretInput(value, scale, ctx.changePassword));
        card->addChild(std::move(r));
      }
      if (!draft->status.empty()) {
        auto r = row(scale);
        r->addChild(ui::label({.text = draft->status, .fontSize = Style::fontSizeCaption * scale,
                               .color = colorSpecFromRole(draft->statusError ? ColorRole::DangerText
                                                                             : ColorRole::Label2)}));
        content.addChild(std::move(r));
      }
      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale,
                              .fillWidth = true});
      actions->addChild(plainButton(i18n::tr(draft->busy ? "settings.privacy.password-changing"
                                                         : "settings.privacy.password-change"),
                                    ButtonVariant::Primary, scale, draft->busy ? std::function<void()>([] {})
                                                                               : ctx.changePassword));
      content.addChild(std::move(actions));
    }

    void addAccessPage(Flex& content, const SettingsPrivacyContext& ctx, const AccessKindView& kind) {
      const float scale = ctx.scale;
      if (kind.apps.empty()) {
        addEmptyState(content, kind.glyph, i18n::tr("settings.privacy.access-none"), scale);
        return;
      }
      Flex* card = addCard(content, scale);
      for (const auto& app : kind.apps) {
        auto r = row(scale);
        r->addChild(title(app.label, scale));
        if (kind.tokens) {
          r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                  [set = ctx.setAccess, table = kind.table, id = app.id] { set(table, id, false); }));
        } else {
          r->addChild(ui::toggle({
              .checked = app.allowed,
              .scale = scale,
              .onChange = [set = ctx.setAccess, table = kind.table, id = app.id](bool on) { set(table, id, on); },
          }));
        }
        card->addChild(std::move(r));
      }
      addStatus(content, ctx);
    }

    std::string countText(std::size_t n, std::string_view one, std::string_view many) {
      return n == 1 ? i18n::tr(one) : i18n::tr(many, "count", std::to_string(n));
    }

    namespace gs = global_shortcuts;

    // Rulebook 9.10 destructive text action: danger_text label, fill_1 hover, fill_2 pressed.
    std::unique_ptr<Node> dangerTextButton(std::string text, float scale, std::function<void()> onClick) {
      const ColorSpec none = clearColorSpec();
      const ColorSpec label = colorSpecFromRole(ColorRole::DangerText);
      const Button::ButtonStateColors rest{.bg = none, .border = none, .label = label};
      const Button::ButtonStateColors hover{.bg = colorSpecFromRole(ColorRole::Fill1), .border = none, .label = label};
      const Button::ButtonStateColors pressed{.bg = colorSpecFromRole(ColorRole::Fill2), .border = none, .label = label};
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .customPalette = Button::ButtonPalette{
              .borderWidth = 0.0F, .normal = rest, .hover = hover, .pressed = pressed, .disabled = rest
          },
          .minHeight = Tokens::sizeControlS * scale,
          .paddingV = 0.0F,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::string shortcutTitle(const gs::Shortcut& shortcut) {
      return shortcut.description.empty() ? shortcut.id : shortcut.description;
    }

    // One shortcut row: what it does, why it does not fire, and its keys or the button that fixes it.
    void addShortcutRow(Flex& card, const SettingsPrivacyContext& ctx, const gs::App& app, const gs::Shortcut& shortcut) {
      const float scale = ctx.scale;
      const auto* recording = std::get_if<gs::Recording>(&ctx.shortcuts->edit);
      const bool here = recording != nullptr && recording->app == app.id && recording->id == shortcut.id;
      using enum gs::Shortcut::Conflict;
      std::string note;
      bool bad = true;
      if (here && !recording->refused.empty()) {
        note = recording->holder.empty()
                   ? i18n::tr("settings.privacy.shortcuts-refused-reserved", "keys", recording->refused)
                   : i18n::tr("settings.privacy.shortcuts-refused-in-use", "keys", recording->refused, "app",
                              ctx.appName(recording->holder));
      } else if (here) {
        note = i18n::tr("settings.privacy.shortcuts-recording");
        bad = false;
      } else if (shortcut.conflict == Unassigned) {
        note = i18n::tr("settings.privacy.shortcuts-unassigned");
      } else if (shortcut.conflict == Disabled) {
        note = i18n::tr("settings.privacy.shortcuts-disabled");
      } else if (shortcut.conflict == Reserved) {
        note = i18n::tr("settings.privacy.shortcuts-reserved", "keys", shortcut.trigger);
      } else if (shortcut.conflict == InUse) {
        note = i18n::tr("settings.privacy.shortcuts-in-use", "keys", shortcut.trigger, "app",
                        shortcut.holder.empty() ? i18n::tr("settings.privacy.shortcuts-another-app")
                                                : ctx.appName(shortcut.holder));
      } else if (shortcut.source == gs::Shortcut::Source::Config) {
        note = i18n::tr("settings.privacy.shortcuts-set-by-you");
        bad = false;
      }

      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      r->setMinHeight((note.empty() ? Tokens::sizeRow : Tokens::sizeRowTwo) * scale);
      r->addChild(ui::row({.width = Tokens::sizeSymbol * scale}));
      auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = shortcutTitle(shortcut), .fontSize = Style::fontSizeBody * scale,
                                .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
      if (!note.empty()) {
        text->addChild(ui::label({.text = std::move(note), .fontSize = Tokens::typeFootnote.size * scale,
                                  .color = colorSpecFromRole(bad ? ColorRole::DangerText : ColorRole::Label2),
                                  .maxLines = 1}));
      }
      r->addChild(std::move(text));
      auto record = [event = ctx.shortcutEvent, app = app.id, id = shortcut.id] {
        event(gs::Record{.app = app, .id = id});
      };
      const bool keysShown = here || shortcut.conflict == None || shortcut.source == gs::Shortcut::Source::Config;
      if (keysShown && (here || !shortcut.trigger.empty())) {
        const bool refused = here && !recording->refused.empty();
        r->addChild(makeChordButton(refused ? recording->refused : shortcut.trigger, here,
                                    refused ? ColorRole::DangerText : here ? ColorRole::AccentText : ColorRole::Label2,
                                    scale, std::move(record)));
      } else {
        const bool unset = shortcut.conflict == Unassigned || shortcut.conflict == Disabled;
        r->addChild(plainButton(i18n::tr(unset ? "settings.privacy.shortcuts-set-keys"
                                               : "settings.privacy.shortcuts-choose-keys"),
                                ButtonVariant::Secondary, scale, std::move(record)));
      }
      card.addChild(std::move(r));
    }

    void addFailureRow(Flex& parent, const SettingsPrivacyContext& ctx, std::string text, std::string retryApp) {
      const float scale = ctx.scale;
      auto r = row(scale);
      r->setGap(Style::spaceSm * scale);
      r->addChild(symbol("alert-circle", Tokens::sizeSymbol * scale, ColorRole::DangerText));
      r->addChild(ui::label({.text = std::move(text), .fontSize = Tokens::typeFootnote.size * scale,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2, .flexGrow = 1.0F}));
      r->addChild(plainButton(i18n::tr("settings.privacy.shortcuts-try-again"), ButtonVariant::Ghost, scale,
                              [event = ctx.shortcutEvent, app = std::move(retryApp)] {
                                event(gs::AskRemove{.app = app});
                              }));
      parent.addChild(std::move(r));
    }

    void addShortcutsPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      const gs::State& state = *ctx.shortcuts;
      if (state.load != gs::Load::Ready) {
        return;
      }
      using Kind = gs::State::Failure::Kind;
      if (state.apps.empty()) {
        addEmptyState(content, "keyboard", i18n::tr("settings.privacy.shortcuts-empty"), scale,
                      i18n::tr("settings.privacy.shortcuts-empty-body"),
                      plainButton(i18n::tr("settings.privacy.shortcuts-open-keyboard"), ButtonVariant::Secondary,
                                  scale, ctx.openKeyboardShortcuts));
        if (state.failure && state.failure->kind == Kind::RemoveAll) {
          addFailureRow(content, ctx, i18n::tr("settings.privacy.shortcuts-remove-all-failed"), {});
        }
        return;
      }
      content.addChild(ui::label({.text = i18n::tr("settings.privacy.shortcuts-intro"),
                                  .fontSize = Tokens::typeFootnote.size * scale,
                                  .color = colorSpecFromRole(ColorRole::Label2), .maxWidth = 620.0F * scale,
                                  .maxLines = 3}));

      // Cards start open up to 6 apps, and always while a shortcut does not work; a click flips the default.
      const bool manyApps = state.apps.size() > 6;
      for (const gs::App& app : state.apps) {
        const std::size_t bad = app.notWorking();
        const bool toggled = ctx.toggledShortcutApps != nullptr && ctx.toggledShortcutApps->contains(app.id);
        const bool open = (!manyApps || bad > 0) != toggled;
        Flex* card = addCard(content, scale);

        auto header = row(scale);
        header->setGap(Style::spaceMd * scale);
        header->setMinHeight(Tokens::sizeRowTwo * scale);
        const std::string name = ctx.appName(app.id);
        header->addChild(iconButton(open ? "chevron-down" : "chevron-right", name, scale,
                                    [toggle = ctx.toggleShortcutApp, id = app.id] { toggle(id); }));
        if (auto icon = ctx.appIcon(app.id, Tokens::componentListAppIcon * scale); icon != nullptr) {
          header->addChild(std::move(icon));
        }
        auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
        text->addChild(ui::label({.text = name, .fontSize = Tokens::typeHeadline.size * scale,
                                  .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                                  .maxLines = 1}));
        auto meta = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        meta->addChild(ui::row({.fill = colorSpecFromRole(app.running ? ColorRole::Accent : ColorRole::Label3),
                                .radius = Tokens::componentListStatusDot * 0.5F * scale,
                                .width = Tokens::componentListStatusDot * scale,
                                .height = Tokens::componentListStatusDot * scale}));
        meta->addChild(makeLabel(
            i18n::tr(app.running ? "settings.privacy.shortcuts-running" : "settings.privacy.shortcuts-not-running")
                + " · "
                + countText(app.shortcuts.size(), "settings.privacy.shortcuts-one", "settings.privacy.shortcuts-many"),
            Tokens::typeFootnote.size * scale, colorSpecFromRole(ColorRole::Label2)
        ));
        if (bad > 0) {
          meta->addChild(makeLabel("· " + i18n::tr("settings.privacy.shortcuts-not-working", "count",
                                                   std::to_string(bad)),
                                   Tokens::typeFootnote.size * scale, colorSpecFromRole(ColorRole::DangerText)));
        }
        text->addChild(std::move(meta));
        header->addChild(std::move(text));
        header->addChild(dangerTextButton(i18n::tr("settings.privacy.shortcuts-remove"), scale,
                                          [event = ctx.shortcutEvent, id = app.id] { event(gs::AskRemove{.app = id}); }));
        card->addChild(std::move(header));

        if (open) {
          for (const gs::Shortcut& shortcut : app.shortcuts) {
            addShortcutRow(*card, ctx, app, shortcut);
          }
        }
        if (state.failure && state.failure->app == app.id) {
          if (state.failure->kind == Kind::Remove) {
            addFailureRow(*card, ctx, i18n::tr("settings.privacy.shortcuts-remove-failed", "app", name), app.id);
          } else {
            auto r = row(scale);
            r->addChild(ui::label({.text = i18n::tr("settings.privacy.shortcuts-save-failed"),
                                   .fontSize = Tokens::typeFootnote.size * scale,
                                   .color = colorSpecFromRole(ColorRole::DangerText), .maxLines = 2}));
            card->addChild(std::move(r));
          }
        }
        addSettingsRowSeparators(*card, scale);
      }

      if (state.failure && state.failure->kind == Kind::RemoveAll) {
        addFailureRow(content, ctx, i18n::tr("settings.privacy.shortcuts-remove-all-failed"), {});
      }
      auto end = ui::row({.justify = FlexJustify::End, .fillWidth = true});
      end->addChild(dangerTextButton(i18n::tr("settings.privacy.shortcuts-remove-all"), scale,
                                     [event = ctx.shortcutEvent] { event(gs::AskRemove{.app = {}}); }));
      content.addChild(std::move(end));
    }

    void addTrustedAppsPage(Flex& content, const SettingsPrivacyContext& ctx) {
      const float scale = ctx.scale;
      auto add = plainButton(i18n::tr("settings.privacy.screen-input-add"), ButtonVariant::Secondary, scale,
                             ctx.addTrustedApp);
      if (ctx.trustedApps->paths.empty()) {
        addEmptyState(content, "pointer", i18n::tr("settings.privacy.screen-input-none"), scale,
                      i18n::tr("settings.privacy.screen-input-body"), std::move(add));
        addStatus(content, ctx);
        return;
      }
      Flex* card = addCard(content, scale);
      for (const auto& path : ctx.trustedApps->paths) {
        auto r = row(scale);
        r->setGap(Style::spaceMd * scale);
        r->setMinHeight(Tokens::sizeRowTwo * scale);
        r->addChild(symbol("app-window", Style::fontSizeBody * scale, ColorRole::Label2));
        auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
        text->addChild(ui::label({.text = std::filesystem::path(path).filename().string(),
                                  .fontSize = Style::fontSizeBody * scale,
                                  .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
        text->addChild(ui::label({.text = path, .fontSize = Tokens::typeFootnote.size * scale,
                                  .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
        r->addChild(std::move(text));
        r->addChild(plainButton(i18n::tr("settings.privacy.remove"), ButtonVariant::Destructive, scale,
                                [remove = ctx.removeTrustedApp, path] { remove(path); }));
        card->addChild(std::move(r));
      }
      auto actions = row(scale);
      actions->addChild(std::move(add));
      card->addChild(std::move(actions));
      addStatus(content, ctx);
      auto note = row(scale);
      note->addChild(ui::label({.text = i18n::tr("settings.privacy.screen-input-note"),
                                .fontSize = Style::fontSizeCaption * scale,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      content.addChild(std::move(note));
    }
  } // namespace

  void addSettingsPrivacy(Flex& content, const SettingsPrivacyContext& ctx) {
    const float scale = ctx.scale;
    // "privacy:<title>\t<kind>": the title shows in the page header, the kind picks the page.
    if (const auto tab = ctx.detail.find('\t'); ctx.detail.starts_with("privacy:") && tab != std::string::npos) {
      const std::string kind = ctx.detail.substr(tab + 1);
      if (kind == "face") {
        addFacePage(content, ctx);
      } else if (kind == "finger") {
        addFingerPage(content, ctx);
      } else if (kind == "password" && ctx.password != nullptr) {
        addPasswordPage(content, ctx);
      } else if (kind.starts_with("access:")) {
        const std::string table = kind.substr(7);
        if (const auto it = std::ranges::find(ctx.access, table, &AccessKindView::table); it != ctx.access.end()) {
          addAccessPage(content, ctx, *it);
        }
      } else if (kind == "global-shortcuts" && ctx.shortcuts != nullptr) {
        addShortcutsPage(content, ctx);
      } else if (kind == "trusted-apps" && ctx.trustedApps != nullptr
                 && ctx.trustedApps->load == TrustedAppsView::Load::Ready) {
        addTrustedAppsPage(content, ctx);
      } else if (kind == "rename") {
        addRenamePage(content, ctx, ctx.detail.substr(8, tab - 8));
      }
      return;
    }

    Flex* card = addSection(content, i18n::tr("settings.privacy.sign-in"), nullptr, scale);
    const std::string unavailable = i18n::tr("settings.privacy.not-available");
    std::string face = unavailable;
    if (ctx.facePresent) {
      face = ctx.faces.empty() ? i18n::tr("settings.privacy.not-set-up")
                               : countText(ctx.faces.size(), "settings.privacy.faces-one", "settings.privacy.faces-many");
      if (!ctx.faces.empty() && ctx.faceOnRequest) {
        face += " · " + i18n::tr("settings.privacy.on-tap");
      }
    }
    std::string finger = unavailable;
    if (ctx.fingerPresent) {
      finger = ctx.fingers.empty()
                   ? i18n::tr("settings.privacy.not-set-up")
                   : countText(ctx.fingers.size(), "settings.privacy.fingers-one", "settings.privacy.fingers-many");
    }
    if (ctx.password != nullptr) {
      const std::string passwordTitle = i18n::tr("settings.privacy.password");
      addLinkRow(*card, "key", passwordTitle, {}, "privacy:" + passwordTitle + "\tpassword", true, ctx);
    }
    const std::string faceTitle = i18n::tr("settings.privacy.face-unlock");
    const std::string fingerTitle = i18n::tr("settings.privacy.fingerprint");
    addLinkRow(*card, "face-id", faceTitle, std::move(face), "privacy:" + faceTitle + "\tface", ctx.biometricsKnown,
               ctx);
    addLinkRow(*card, "fingerprint", fingerTitle, std::move(finger), "privacy:" + fingerTitle + "\tfinger",
               ctx.biometricsKnown, ctx);

    const bool shortcutsRow = ctx.shortcuts != nullptr && ctx.shortcuts->load == global_shortcuts::Load::Ready;
    const bool trustedRow = ctx.trustedApps != nullptr && ctx.trustedApps->load == TrustedAppsView::Load::Ready;
    if (!ctx.access.empty() || shortcutsRow || trustedRow) {
      Flex* access = addSection(content, i18n::tr("settings.privacy.app-access"), nullptr, scale);
      if (trustedRow) {
        const std::size_t apps = ctx.trustedApps->paths.size();
        const std::string trustedTitle = i18n::tr("settings.privacy.screen-input");
        addLinkRow(*access, "pointer", trustedTitle,
                   apps == 0 ? i18n::tr("settings.privacy.access-no-apps")
                             : countText(apps, "settings.privacy.apps-one", "settings.privacy.apps-many"),
                   "privacy:" + trustedTitle + "\ttrusted-apps", true, ctx);
      }
      if (shortcutsRow) {
        const std::size_t apps = ctx.shortcuts->apps.size();
        const std::string shortcutsTitle = i18n::tr("settings.privacy.global-shortcuts");
        addLinkRow(*access, "keyboard", shortcutsTitle,
                   apps == 0 ? i18n::tr("settings.privacy.access-no-apps")
                             : countText(apps, "settings.privacy.apps-one", "settings.privacy.apps-many"),
                   "privacy:" + shortcutsTitle + "\tglobal-shortcuts", true, ctx);
      }
      for (const auto& kind : ctx.access) {
        const auto allowed = std::ranges::count_if(kind.apps, [](const auto& app) { return app.allowed; });
        std::string value = !kind.loaded ? std::string{}
                            : allowed == 0 ? i18n::tr("settings.privacy.access-no-apps")
                                           : countText(static_cast<std::size_t>(allowed), "settings.privacy.apps-one",
                                                       "settings.privacy.apps-many");
        const std::string kindTitle = i18n::tr(kind.titleKey);
        addLinkRow(*access, kind.glyph, kindTitle, std::move(value), "privacy:" + kindTitle + "\taccess:" + kind.table,
                   kind.loaded, ctx);
      }
      auto note = row(scale);
      note->addChild(ui::label({.text = i18n::tr("settings.privacy.microphone-note"),
                                .fontSize = Style::fontSizeCaption * scale,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      content.addChild(std::move(note));
    }
  }

} // namespace settings
