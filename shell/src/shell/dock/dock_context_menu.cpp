#include "shell/dock/dock_context_menu.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/dock/pinned_apps.h"
#include "system/desktop_entry.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "compositors/wayland_toplevels.h"

#include <algorithm>
#include <cstdint>

namespace shell::dock {
  namespace {

    constexpr Logger kLog("dock");

    constexpr float kMenuWidth = 240.0F;
    constexpr std::int32_t kMenuCloseId = -1;
    constexpr std::int32_t kMenuCloseAllId = -2;
    constexpr std::int32_t kMenuSeparatorId = -3;
    constexpr std::int32_t kMenuPinToggleId = -4;
    constexpr std::int32_t kMenuOpenTrashId = -5;
    constexpr std::int32_t kMenuHideTrashId = -6;

    bool isWindowClosable(const ToplevelInfo& window) {
      if (window.handle != nullptr) {
        return true;
      }
      // Exact-identity ext toplevels close through `closeToplevelInfo` via the backend, without a wlr handle.
      return window.exactIdentity && !window.identifier.empty();
    }

    popup_chrome::Attachment popupAttachmentForDockPosition(bool isBottom, bool isTop, bool isRight) {
      if (isBottom) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center,
            .vertical = popup_chrome::VerticalAttachment::Bottom
        };
      }
      if (isTop) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
        };
      }
      if (isRight) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Right,
            .vertical = popup_chrome::VerticalAttachment::Center
        };
      }
      return popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Left, .vertical = popup_chrome::VerticalAttachment::Center
      };
    }

  } // namespace

  DockPopup::DockPopup() = default;
  DockPopup::~DockPopup() = default;

  bool routePopupEvent(DockPopup& popup, const PointerEvent& event) {
    return popup.menu != nullptr && popup.menu->isOpen() && popup.menu->onPointerEvent(event);
  }

  namespace {
    // Shows `entries` as a dock popup menu at the pointer; `onActivate` gets the chosen entry id, then the menu closes.
    std::unique_ptr<DockPopup> showMenu(
        std::unique_ptr<DockPopup> menu, CompositorPlatform& platform, ConfigService& config,
        RenderContext& renderContext, Surface* parent, wl_output* output, const DockConfig& dockConfig,
        std::vector<ContextMenuControlEntry> entries, const std::function<void(std::int32_t)>& onActivate,
        const std::function<void()>& closeMenu
    ) {
      if (parent == nullptr) {
        kLog.warn("dock: no dock surface for the item menu");
        return nullptr;
      }
      const DockEdge edge = dockConfig.position;
      const bool isBottom = edge == DockEdge::Bottom;
      const bool isTop = edge == DockEdge::Top;
      const bool isRight = edge == DockEdge::Right;
      // Clearance past the icon/tooltip; chrome Bottom attachment folds bleed into offset.
      const std::int32_t kGap = std::max(2, static_cast<std::int32_t>(Style::spaceLg + Style::spaceMd));
      ContextMenuPopupPlacement placement{
          .chromeAttachment = popupAttachmentForDockPosition(isBottom, isTop, isRight),
      };
      if (isBottom) {
        placement.anchor = PopupAnchor::Top;
        placement.gravity = PopupGravity::Top;
        placement.offsetY = -kGap;
      } else if (isTop) {
        placement.anchor = PopupAnchor::Bottom;
        placement.gravity = PopupGravity::Bottom;
        placement.offsetY = kGap;
      } else if (isRight) {
        placement.anchor = PopupAnchor::Left;
        placement.gravity = PopupGravity::Left;
        placement.offsetX = -kGap;
      } else {
        placement.anchor = PopupAnchor::Right;
        placement.gravity = PopupGravity::Right;
        placement.offsetX = kGap;
      }

      // Pointer-centred cell (tray-style); panel-face anchors miss hover-zoom padding.
      const auto ptrX = static_cast<std::int32_t>(platform.lastPointerX());
      const auto ptrY = static_cast<std::int32_t>(platform.lastPointerY());
      const std::int32_t halfCell = std::max(1, dockConfig.iconSize / 2);

      menu->menu = std::make_unique<ContextMenuPopup>(platform.wayland(), renderContext);
      menu->menu->setShadowConfig(config.config().shell.shadow);
      menu->menu->setOnActivate([onActivate](const ContextMenuControlEntry& e) { onActivate(e.id); });
      menu->menu->setOnDismissed([closeMenu] {
        DeferredCall::callLater([closeMenu] {
          if (closeMenu) {
            closeMenu();
          }
        });
      });
      menu->menu->open(ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = kMenuWidth,
          .maxMenuWidth = Style::menuAutoMaxWidth,
          .anchor = PopupAnchorRect{.x = ptrX - halfCell, .y = ptrY - halfCell, .width = 2 * halfCell, .height = 2 * halfCell},
          .parent = PopupParent{.surface = parent, .output = platform.wayland().outputId(output)},
          .inputSerial = platform.lastInputSerial(),
          .placement = placement,
      });
      return menu;
    }
  } // namespace

  std::unique_ptr<DockPopup> createItemMenu(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      Surface* parent, wl_output* output, const DockConfig& dockConfig,
      const DesktopEntry& entry, const std::vector<ToplevelInfo>& windows, const DockMenuCallbacks& callbacks
  ) {
    auto menu = std::make_unique<DockPopup>();

    // Collect running windows for the close actions.
    menu->windows = windows;

    // IDs 0..N-1 -> desktop actions; negative constants -> pin and close commands.
    std::vector<ContextMenuControlEntry> entries;
    entries.reserve(entry.actions.size() + 5);

    const bool isPinned = pinned_apps::containsEntry(dockConfig.pinned, entry);
    entries.push_back(
        ContextMenuControlEntry{
            .id = kMenuPinToggleId,
            .label = i18n::tr(isPinned ? "tray.menu.unpin" : "tray.menu.pin"),
            .enabled = callbacks.setEntryPinned != nullptr,
            .separator = false,
            .hasSubmenu = false,
        }
    );

    std::vector<ContextMenuControlEntry> bodyEntries;
    bodyEntries.reserve(entry.actions.size() + 4);

    const bool hasActionEntries = !entry.actions.empty();
    std::vector<std::size_t> closableWindowIndices;
    closableWindowIndices.reserve(menu->windows.size());
    for (std::size_t i = 0; i < menu->windows.size(); ++i) {
      if (isWindowClosable(menu->windows[i])) {
        closableWindowIndices.push_back(i);
      }
    }
    const std::size_t closableCount = closableWindowIndices.size();

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(entry.actions.size()); ++i) {
      bodyEntries.push_back(
          ContextMenuControlEntry{
              .id = i,
              .label = entry.actions[static_cast<std::size_t>(i)].name,
              .enabled = true,
              .separator = false,
              .hasSubmenu = false,
          }
      );
    }

    if (closableCount > 0) {
      if (hasActionEntries) {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false
            }
        );
      }
      if (closableCount == 1) {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuCloseId,
                .label = i18n::tr("dock.actions.close"),
                .enabled = true,
                .separator = false,
                .hasSubmenu = false
            }
        );
      } else {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuCloseAllId,
                .label = i18n::tr("dock.actions.close-all"),
                .enabled = true,
                .separator = false,
                .hasSubmenu = false
            }
        );
      }
    }

    if (!bodyEntries.empty()) {
      entries.push_back(
          ContextMenuControlEntry{
              .id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false
          }
      );
      entries.insert(entries.end(), bodyEntries.begin(), bodyEntries.end());
    }

    if (entries.empty()) {
      return nullptr;
    }

    // Capture by value; the entry may be rebuilt before the callback fires.
    auto onActivate = [entryActions = entry.actions, callbacks, isPinned, menuWindows = menu->windows,
                       closableWindowIndices](std::int32_t id) {
      if (id == kMenuPinToggleId) {
        if (callbacks.setEntryPinned) {
          callbacks.setEntryPinned(!isPinned);
        }
      } else if (id >= 0) {
        const auto idx = static_cast<std::size_t>(id);
        if (idx < entryActions.size() && callbacks.launchAction) {
          callbacks.launchAction(entryActions[idx]);
        }
      } else if (id == kMenuCloseId && !closableWindowIndices.empty()) {
        if (callbacks.closeWindow) {
          callbacks.closeWindow(closableWindowIndices[0]);
        }
      } else if (id == kMenuCloseAllId) {
        if (callbacks.closeWindow) {
          for (std::size_t windowIndex : closableWindowIndices) {
            if (windowIndex < menuWindows.size()) {
              callbacks.closeWindow(windowIndex);
            }
          }
        }
      }
    };
    return showMenu(
        std::move(menu), platform, config, renderContext, parent, output, dockConfig, entries, onActivate,
        callbacks.closeMenu
    );
  }

  std::unique_ptr<DockPopup> createTrashMenu(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      Surface* parent, wl_output* output, const DockConfig& dockConfig,
      const DockTrashMenuCallbacks& callbacks
  ) {
    const std::vector<ContextMenuControlEntry> entries{
        {.id = kMenuOpenTrashId, .label = i18n::tr("dock.trash-menu.open"), .enabled = callbacks.openTrash != nullptr},
        {.id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true},
        {.id = kMenuHideTrashId, .label = i18n::tr("dock.trash-menu.hide"), .enabled = callbacks.hideTrash != nullptr},
    };
    auto onActivate = [callbacks](std::int32_t id) {
      if (id == kMenuOpenTrashId && callbacks.openTrash) {
        callbacks.openTrash();
      } else if (id == kMenuHideTrashId && callbacks.hideTrash) {
        callbacks.hideTrash();
      }
    };
    return showMenu(
        std::make_unique<DockPopup>(), platform, config, renderContext, parent, output, dockConfig,
        entries, onActivate, callbacks.closeMenu
    );
  }

} // namespace shell::dock
