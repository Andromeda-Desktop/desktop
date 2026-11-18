#include "app/app_clipboard.h"

#include "core/log.h"
#include "platform/display.h"

#include <vector>

namespace {
  constexpr std::size_t kMaxTextBytes = 1U << 20; // text fields paste up to 1 MiB; a bigger clipboard is not read ahead
  constexpr Logger kLog("app-clipboard");
  const std::vector<std::string> kTextTypes{"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};
}

AppClipboard::AppClipboard(Display& display) : m_display(display) {
  m_display.setClipboardChangedCallback([this] { refresh(); });
}

AppClipboard::~AppClipboard() { m_display.setClipboardChangedCallback({}); }

void AppClipboard::refresh() {
  const unsigned generation = ++m_generation;
  m_display.readClipboard(
      kTextTypes,
      [this, generation, alive = std::weak_ptr<bool>(m_alive)](const std::string& mime, std::string bytes) {
        if (alive.expired() || generation != m_generation) {
          return; // gone, or a newer clipboard arrived while this one was read
        }
        m_text = mime.empty() ? std::nullopt : std::optional(std::move(bytes));
        kLog.debug("clipboard text {}", m_text.has_value() ? std::to_string(m_text->size()) + " bytes" : "none");
      },
      kMaxTextBytes
  );
}

void AppClipboard::setClipboardText(std::string text) {
  if (!m_display.setClipboard(m_display.lastInputSerial(), kTextTypes, [text](const std::string&) { return text; })) {
    return;
  }
  m_text = std::move(text);
  ++m_generation;
}

void AppClipboard::requestClipboardText(std::function<void(std::optional<std::string>)> done) {
  // The read-ahead keeps "" for a clipboard over kMaxTextBytes.
  if (m_text && !m_text->empty()) {
    done(m_text);
    return;
  }
  m_display.readClipboard(
      kTextTypes,
      [done = std::move(done)](const std::string& mime, std::string bytes) {
        done(mime.empty() ? std::nullopt : std::optional(std::move(bytes)));
      },
      static_cast<std::size_t>(-1)
  );
}

std::optional<std::string> AppClipboard::primaryText() { return m_display.primaryText(); }

void AppClipboard::setPrimaryText(std::string text) { m_display.setPrimaryText(m_display.lastInputSerial(), std::move(text)); }

void AppClipboard::requestPrimaryText(std::function<void(std::optional<std::string>)> done) {
  m_display.whenPrimaryText(std::move(done));
}
