#include "ui/dialogs/layer_popup_host.h"

#include "platform/display.h"

void LayerPopupHostRegistry::registerHost(
    ContextResolver contextResolver, PopupHook beginAttachedPopup, PopupHook endAttachedPopup,
    FallbackResolver fallbackResolver
) {
  m_hosts.push_back(
      Host{
          .contextResolver = std::move(contextResolver),
          .beginAttachedPopup = std::move(beginAttachedPopup),
          .endAttachedPopup = std::move(endAttachedPopup),
          .fallbackResolver = std::move(fallbackResolver),
      }
  );
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::contextForSurface(Surface* surface) const {
  if (surface == nullptr) {
    return std::nullopt;
  }

  for (const Host& host : m_hosts) {
    if (!host.contextResolver) {
      continue;
    }
    if (auto context = host.contextResolver(surface); context.has_value()) {
      return context;
    }
  }

  return std::nullopt;
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::fallbackContext() const {
  for (const Host& host : m_hosts) {
    if (!host.fallbackResolver) {
      continue;
    }
    if (auto context = host.fallbackResolver(); context.has_value()) {
      return context;
    }
  }

  return std::nullopt;
}

std::pair<std::int32_t, std::int32_t> LayerPopupParentContext::centeringOffset(const Display& display) const {
  if (!usedFallback) {
    return {0, 0};
  }

  const auto out = display.outputInfo(output);
  if (!out.has_value()) {
    return {0, 0};
  }
  return {
      (out->logicalWidth - static_cast<std::int32_t>(width)) / 2,
      (out->logicalHeight - static_cast<std::int32_t>(height)) / 2,
  };
}

std::optional<LayerPopupParentContext> LayerPopupHostRegistry::resolveForInput(const Display& display) const {
  const auto trySurface = [this](Surface* surface) { return contextForSurface(surface); };

  if (display.lastInputSource() == InputSource::Keyboard) {
    if (auto context = trySurface(display.keyboardSurface()); context.has_value()) {
      return context;
    }
    if (auto context = trySurface(display.pointerSurface()); context.has_value()) {
      return context;
    }
  } else {
    if (auto context = trySurface(display.pointerSurface()); context.has_value()) {
      return context;
    }
    if (auto context = trySurface(display.keyboardSurface()); context.has_value()) {
      return context;
    }
  }

  auto context = fallbackContext();
  if (context.has_value()) {
    context->usedFallback = true;
  }
  return context;
}

void LayerPopupHostRegistry::beginAttachedPopup(Surface* surface) const {
  if (surface == nullptr) {
    return;
  }

  for (const Host& host : m_hosts) {
    if (host.beginAttachedPopup) {
      host.beginAttachedPopup(surface);
    }
  }
}

void LayerPopupHostRegistry::endAttachedPopup(Surface* surface) const {
  if (surface == nullptr) {
    return;
  }

  for (const Host& host : m_hosts) {
    if (host.endAttachedPopup) {
      host.endAttachedPopup(surface);
    }
  }
}
