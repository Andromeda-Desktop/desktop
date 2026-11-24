#pragma once

#include "shell/notification_center/notification_center_model.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

class Button;
class CompositorPlatform;
class Flex;
class Glyph;
class Image;
class InputArea;
class Label;
class NotificationManager;
class ScrollView;
struct NotificationImageData;

// Concept C notification center (proto overlays.js renderNC): a head capsule and app groups of cards, each its own
// glass piece, in a 360 column under the island.
class NotificationCenterPanel final : public Panel {
public:
  NotificationCenterPanel(NotificationManager* notifications, CompositorPlatform* platform)
      : m_notifications(notifications), m_platform(platform) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  // The manager clamps this to the room under the bar; the column itself stops center_reserve above the output bottom.
  [[nodiscard]] float preferredHeight() const override { return scaled(4096.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] bool padsContent() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  // auto: it hangs under the bell like the tray drawer (user, 2026-11-19); a call without a click anchors top centre.
  [[nodiscard]] std::string panelScreenPosition() const override { return "auto"; }

protected:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

private:
  struct Card {
    Flex* node = nullptr;
    InputArea* area = nullptr;
    Flex* clear = nullptr;
    Label* age = nullptr;
  };
  // A back card of a collapsed stack: `node` clips `card` to the strip it shows under the card in front.
  struct Peek {
    Flex* node = nullptr;
    Flex* card = nullptr;
    const Flex* front = nullptr;
    int depth = 1;
  };
  struct IconSlot {
    Image* image = nullptr;
    Glyph* fallback = nullptr;
    std::string icon;
    std::string desktopEntry;
    std::shared_ptr<const NotificationImageData> raw;
    bool done = false;
  };

  [[nodiscard]] std::string contentKey() const;
  void rebuild();
  void captureReflow();
  void collapseGroup(const std::string& key);
  void playReflow();
  void requestRebuild();
  void addGroup(Flex& list, const notification_center::Group& group);
  [[nodiscard]] std::unique_ptr<Flex> makeCard(const Notification& n);
  void activate(std::uint32_t id, const std::string& action);
  void loadIcons(Renderer& renderer);

  NotificationManager* m_notifications = nullptr;
  CompositorPlatform* m_platform = nullptr;
  IconResolver m_iconResolver{true};
  std::optional<int> m_eventToken;

  Flex* m_column = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_list = nullptr;
  std::vector<Card> m_cards;
  std::vector<Peek> m_peeks;
  // Reflow (rulebook P13): every card, peek and footer under a stable key; a card new to the layout starts at
  // `from`'s place (its group's front card), so an expanded stack slides out from under it.
  struct Moving {
    std::string key;
    std::string from;
    Node* node = nullptr;
  };
  std::vector<Moving> m_moving;
  // Scene y of each key before a group toggle; the next layout after the rebuild slides every node from it.
  std::unordered_map<std::string, float> m_reflowFrom;
  std::vector<IconSlot> m_icons;
  std::set<std::string> m_openGroups;
  std::string m_key;
  bool m_dirty = false;
  float m_slotHeight = 0.0F;
  float m_maxHeight = 0.0F;
};
