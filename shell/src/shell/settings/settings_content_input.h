#pragma once

#include "system/xkb_layout_catalog.h"
#include "compositors/settings_control.h"

#include <functional>
#include <string>

class Flex;

namespace settings {

  class SettingsControlFactory;

  struct SettingsInputContext {
    float scale = 1.0F;
    const SettingsControl* input = nullptr;
    const xkb::Catalog* catalog = nullptr;
    std::function<void(std::string key, std::string value)> set;
    SettingsControlFactory* factory = nullptr;
    // Mouse page: the mouse and trackpad cards; Keyboard page: the keyboard card.
    bool pointer = false;
  };

  void addSettingsInput(Flex& content, const SettingsInputContext& ctx);

} // namespace settings
