#include "osc99.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace term {

namespace {

constexpr std::size_t kMaxField = 16384;
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kMaxButtons = 8;


void append(std::string& field, const std::string& text) {
  field.append(text, 0, kMaxField - std::min(kMaxField, field.size()));
}

} // namespace

void Osc99Scanner::feed(std::span<const std::uint8_t> bytes, const std::function<void(Event)>& emit) {
  m_frame.feed(bytes, [&](std::string_view payload) { finish(payload, emit); });
}

void Osc99Scanner::finish(std::string_view payload, const std::function<void(Event)>& emit) {
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
