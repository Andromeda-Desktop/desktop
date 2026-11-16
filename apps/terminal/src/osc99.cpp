#include "osc99.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace term {

namespace {

constexpr std::uint8_t kEsc = 0x1b;
constexpr std::uint8_t kBel = 0x07;
constexpr std::uint8_t kCan = 0x18;
constexpr std::uint8_t kSub = 0x1a;
constexpr std::string_view kPrefix = "99;";
constexpr std::size_t kMaxPayload = 8192;
constexpr std::size_t kMaxField = 16384;
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kMaxButtons = 8;

std::string base64Decode(std::string_view in) {
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char c : in) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  return out;
}

void append(std::string& field, const std::string& text) {
  field.append(text, 0, kMaxField - std::min(kMaxField, field.size()));
}

} // namespace

void Osc99Scanner::feed(std::span<const std::uint8_t> bytes, const std::function<void(Event)>& emit) {
  for (std::size_t at = 0; at < bytes.size();) {
    const std::uint8_t b = bytes[at];
    bool consumed = true;
    switch (m_state) {
      case State::Ground:
        if (b == kEsc) m_state = State::Escape;
        break;
      case State::Escape:
        if (b == ']') {
          m_state = State::Prefix;
          m_prefixAt = 0;
        } else if (b != kEsc) {
          m_state = State::Ground;
        }
        break;
      case State::Prefix:
        if (b == static_cast<std::uint8_t>(kPrefix[m_prefixAt])) {
          if (++m_prefixAt == kPrefix.size()) {
            m_state = State::Payload;
            m_payload.clear();
          }
        } else {
          m_state = b == kEsc ? State::Escape : State::Ground;
        }
        break;
      case State::Payload:
        if (b == kBel) {
          m_state = State::Ground;
          finish(emit);
        } else if (b == kEsc) {
          m_state = State::PayloadEscape;
        } else if (b == kCan || b == kSub) {
          m_state = State::Ground;
        } else if (m_payload.size() < kMaxPayload) {
          m_payload.push_back(static_cast<char>(b));
        } else {
          m_state = State::Skip;
        }
        break;
      case State::PayloadEscape:
      case State::SkipEscape:
        if (b == '\\') {
          const bool complete = m_state == State::PayloadEscape;
          m_state = State::Ground;
          if (complete) finish(emit);
        } else {
          // ESC cancels the OSC and starts a new sequence with this byte.
          m_state = State::Escape;
          consumed = false;
        }
        break;
      case State::Skip:
        if (b == kBel || b == kCan || b == kSub) m_state = State::Ground;
        else if (b == kEsc) m_state = State::SkipEscape;
        break;
    }
    if (consumed) ++at;
  }
}

void Osc99Scanner::finish(const std::function<void(Event)>& emit) {
  const std::string_view payload = m_payload;
  const std::size_t split = payload.find(';');
  const std::string_view meta = payload.substr(0, split);
  const std::string_view data = split == std::string_view::npos ? std::string_view{} : payload.substr(split + 1);
  std::string key;
  std::string_view type = "title";
  bool done = true;
  bool encoded = false;
  std::vector<std::pair<char, std::string_view>> options;
  for (std::size_t from = 0; from <= meta.size();) {
    const std::size_t end = std::min(meta.find(':', from), meta.size());
    const std::string_view kv = meta.substr(from, end - from);
    from = end + 1;
    if (kv.size() < 2 || kv[1] != '=') continue;
    const std::string_view value = kv.substr(2);
    switch (kv[0]) {
      case 'i':
        // Kitty's id alphabet; the id is echoed back to the program in replies.
        key.clear();
        for (const char c : value) {
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+' || c == '.') key.push_back(c);
        }
        break;
      case 'p': type = value; break;
      case 'd': done = value != "0"; break;
      case 'e': encoded = value == "1"; break;
      default: options.emplace_back(kv[0], value); break;
    }
  }
  if (type == "?") {
    emit(Query{std::move(key)});
    return;
  }
  if (type == "close") {
    m_pending.erase(key);
    if (!key.empty()) emit(Close{std::move(key)});
    return;
  }
  if (!m_pending.contains(key) && m_pending.size() >= kMaxPending) return;
  Notice& notice = m_pending[key];
  for (const auto& [option, value] : options) {
    switch (option) {
      case 'u':
        if (value == "0" || value == "1" || value == "2") notice.urgency = static_cast<std::uint8_t>(value[0] - '0');
        break;
      case 'o':
        notice.when = value == "unfocused" ? Notice::When::Unfocused
                      : value == "invisible" ? Notice::When::Invisible
                                             : Notice::When::Always;
        break;
      case 'c': notice.reportClose = value == "1"; break;
      case 'a':
        for (std::size_t from = 0; from <= value.size();) {
          const std::size_t end = std::min(value.find(',', from), value.size());
          std::string_view action = value.substr(from, end - from);
          from = end + 1;
          const bool on = !action.starts_with('-');
          if (!on) action.remove_prefix(1);
          if (action == "focus") notice.focus = on;
          else if (action == "report") notice.report = on;
        }
        break;
      default: break;
    }
  }
  const std::string text = encoded ? base64Decode(data) : std::string(data);
  if (type == "title") {
    append(notice.title, text);
  } else if (type == "body") {
    append(notice.body, text);
  } else if (type == "buttons") {
    constexpr std::string_view kSeparator = " ";
    std::string_view rest = text;
    while (!rest.empty() && notice.buttons.size() < kMaxButtons) {
      const std::size_t end = std::min(rest.find(kSeparator), rest.size());
      if (end > 0) notice.buttons.emplace_back(rest.substr(0, std::min<std::size_t>(end, 256)));
      rest.remove_prefix(std::min(rest.size(), end + kSeparator.size()));
    }
  }
  if (!done) return;
  Notice finished = std::move(notice);
  m_pending.erase(key);
  finished.key = std::move(key);
  if (!finished.title.empty() || !finished.body.empty()) emit(Show{std::move(finished)});
}

std::string Osc99Scanner::queryReply(const std::string& key) {
  return "\x1b]99;i=" + (key.empty() ? std::string("0") : key)
         + ":p=?;a=focus,report:o=always,unfocused,invisible:u=0,1,2:p=title,body,buttons,close:c=1\x1b\\";
}

std::string Osc99Scanner::activatedReply(const std::string& key, int button) {
  return "\x1b]99;i=" + (key.empty() ? std::string("0") : key) + ";" + (button > 0 ? std::to_string(button) : "")
         + "\x1b\\";
}

std::string Osc99Scanner::closedReply(const std::string& key) {
  return "\x1b]99;i=" + (key.empty() ? std::string("0") : key) + ":p=close;\x1b\\";
}

} // namespace term
