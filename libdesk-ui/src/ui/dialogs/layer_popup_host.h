#pragma once

#include "platform/output_id.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class Display;
class Surface;

struct LayerPopupParentContext {
  Surface* surface = nullptr;
  OutputId output = OutputId::None;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool usedFallback = false;

  [[nodiscard]] std::pair<std::int32_t, std::int32_t> centeringOffset(const Display& display) const;
};

class LayerPopupHostRegistry {
public:
  using ContextResolver = std::function<std::optional<LayerPopupParentContext>(Surface*)>;
  using PopupHook = std::function<void(Surface*)>;
  using FallbackResolver = std::function<std::optional<LayerPopupParentContext>()>;

  void registerHost(
      ContextResolver contextResolver, PopupHook beginAttachedPopup = {}, PopupHook endAttachedPopup = {},
      FallbackResolver fallbackResolver = {}
  );

  [[nodiscard]] std::optional<LayerPopupParentContext> contextForSurface(Surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackContext() const;
  [[nodiscard]] std::optional<LayerPopupParentContext> resolveForInput(const Display& display) const;

  void beginAttachedPopup(Surface* surface) const;
  void endAttachedPopup(Surface* surface) const;

private:
  struct Host {
    ContextResolver contextResolver;
    PopupHook beginAttachedPopup;
    PopupHook endAttachedPopup;
    FallbackResolver fallbackResolver;
  };

  std::vector<Host> m_hosts;
};
