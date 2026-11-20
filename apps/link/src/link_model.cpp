#include "link_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <tuple>

namespace linkapp {

  Presence presenceFrom(bool daemon, bool pairing, const std::vector<Phone>& phones) {
    using K = Presence::Kind;
    if (!daemon) return {K::NoDaemon, {}};
    if (pairing) return {K::Pairing, {}};
    if (phones.empty()) return {K::NotPaired, {}};
    const auto live = std::ranges::find_if(phones, [](const Phone& p) { return p.connected; });
    if (live == phones.end()) return {K::Away, phones.front()};
    return {live->locked == 1 ? K::Locked : K::Connected, *live};
  }

  bool transferMayMove(Transfer from, Transfer to) {
    switch (from) {
    case Transfer::Offered: return to == Transfer::Running || to == Transfer::Failed;
    case Transfer::Running: return to == Transfer::Running || to == Transfer::Done || to == Transfer::Failed;
    case Transfer::Failed: return to == Transfer::Running;
    case Transfer::Done: return false;
    }
    return false;
  }

  std::optional<std::size_t> Feed::find(const std::string& key) const {
    const auto it = std::ranges::find(m_items, key, &FeedItem::key);
    if (it == m_items.end()) return std::nullopt;
    return static_cast<std::size_t>(it - m_items.begin());
  }

  void Feed::upsert(FeedItem item) {
    if (const auto at = find(item.key)) m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(*at));
    const auto pos = std::ranges::find_if(m_items, [&](const FeedItem& it) { return it.atMs <= item.atMs; });
    m_items.insert(pos, std::move(item));
    if (m_items.size() > kMax) m_items.resize(kMax);
  }

  bool Feed::remove(const std::string& key) {
    const auto at = find(key);
    if (!at) return false;
    m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(*at));
    return true;
  }

  bool Feed::moveTransfer(const std::string& key, Transfer to, std::uint64_t bytes, std::uint64_t total) {
    const auto at = find(key);
    if (!at) return false;
    FeedItem& item = m_items[*at];
    if (item.kind != FeedItem::Kind::Offer || !transferMayMove(item.transfer, to)) return false;
    item.transfer = to;
    if (total > 0) {
      item.bytes = bytes;
      item.total = total;
    }
    return true;
  }

  std::size_t Feed::earlierStart(std::int64_t nowMs) const {
    const auto it = std::ranges::find_if(m_items, [&](const FeedItem& item) { return nowMs - item.atMs >= kNowMs; });
    return static_cast<std::size_t>(it - m_items.begin());
  }

  std::string dayLabel(std::int64_t atMs, std::int64_t nowMs) {
    const auto local = [](std::int64_t ms) {
      const auto t = static_cast<std::time_t>(ms / 1000);
      std::tm tm{};
      localtime_r(&t, &tm);
      return tm;
    };
    const std::tm at = local(atMs);
    std::tm now = local(nowMs);
    // Whole local days between the two dates; mktime normalizes the midnights across DST.
    std::tm a = at;
    a.tm_hour = now.tm_hour = 12;
    a.tm_min = now.tm_min = a.tm_sec = now.tm_sec = 0;
    a.tm_isdst = now.tm_isdst = -1;
    const auto days = static_cast<long>(std::lround(std::difftime(std::mktime(&now), std::mktime(&a)) / 86400.0));
    static constexpr std::array kDays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static constexpr std::array kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (days == 0) return "Today";
    if (days == 1) return "Yesterday";
    if (days > 1 && days < 7) return kDays[static_cast<std::size_t>(at.tm_wday)];
    if (at.tm_year == now.tm_year) return std::format("{} {}", at.tm_mday, kMonths[static_cast<std::size_t>(at.tm_mon)]);
    return std::format("{} {} {}", at.tm_mday, kMonths[static_cast<std::size_t>(at.tm_mon)], at.tm_year + 1900);
  }

  std::string numberKey(const std::string& address) {
    std::string digits;
    for (const char c : address) {
      if (c >= '0' && c <= '9') digits += c;
    }
    if (digits.empty()) return address;
    return digits.size() > 9 ? digits.substr(digits.size() - 9) : digits;
  }

  namespace {
    // The phone stamps a sent text with its own clock; an older text with the same body is not this one.
    constexpr std::int64_t kClockSkewMs = 5LL * 60 * 1000;
  } // namespace

  void Messages::setTexts(std::vector<Text> texts) {
    m_texts = std::move(texts);
    std::ranges::sort(m_texts, [](const Text& a, const Text& b) { return std::tie(a.atMs, a.id) < std::tie(b.atMs, b.id); });
    m_load = Load::Ready;
    std::erase_if(m_outgoing, [&](const Outgoing& out) {
      return std::ranges::any_of(m_texts, [&](const Text& t) {
        return t.sent && t.thread == out.thread && t.body == out.body && t.atMs >= out.atMs - kClockSkewMs;
      });
    });
    readViewed();
    rebuild();
  }

  void Messages::setContacts(const std::vector<Contact>& contacts) {
    m_names.clear();
    for (const Contact& contact : contacts) {
      for (const std::string& number : contact.numbers) m_names.try_emplace(numberKey(number), contact.name);
    }
    rebuild();
  }

  void Messages::received(std::uint64_t textId) {
    m_unread.insert(textId);
    readViewed();
    rebuild();
  }

  void Messages::view(std::optional<std::uint64_t> thread) {
    m_viewing = thread;
    readViewed();
    rebuild();
  }

  std::uint64_t Messages::send(std::uint64_t thread, std::string body, std::int64_t atMs) {
    if (body.empty() || this->thread(thread) == nullptr) return 0;
    m_outgoing.push_back({.serial = ++m_serial, .thread = thread, .body = std::move(body), .atMs = atMs});
    return m_serial;
  }

  bool Messages::sendFailed(std::uint64_t serial) {
    const auto it = std::ranges::find(m_outgoing, serial, &Outgoing::serial);
    if (it == m_outgoing.end() || it->state != Outgoing::State::Sending) return false;
    it->state = Outgoing::State::Failed;
    return true;
  }

  bool Messages::retry(std::uint64_t serial) {
    const auto it = std::ranges::find(m_outgoing, serial, &Outgoing::serial);
    if (it == m_outgoing.end() || it->state != Outgoing::State::Failed) return false;
    it->state = Outgoing::State::Sending;
    return true;
  }

  const Thread* Messages::thread(std::uint64_t id) const {
    const auto it = std::ranges::find(m_threads, id, &Thread::id);
    return it == m_threads.end() ? nullptr : &*it;
  }

  const Outgoing* Messages::outgoing(std::uint64_t serial) const {
    const auto it = std::ranges::find(m_outgoing, serial, &Outgoing::serial);
    return it == m_outgoing.end() ? nullptr : &*it;
  }

  std::string Messages::nameFor(const std::string& address) const {
    const auto it = m_names.find(numberKey(address));
    return it == m_names.end() ? address : it->second;
  }

  int Messages::unread() const {
    int n = 0;
    for (const Thread& t : m_threads) n += t.unread;
    return n;
  }

  // An unread id stays until its text shows up in a viewed thread: TextReceived can come before Texts holds it.
  void Messages::readViewed() {
    if (!m_viewing) return;
    for (const Text& t : m_texts) {
      if (t.thread == *m_viewing) m_unread.erase(t.id);
    }
  }

  void Messages::rebuild() {
    std::map<std::uint64_t, Thread> byId;
    for (const Text& t : m_texts) { // oldest first: the last write per thread is its newest text
      Thread& thread = byId[t.thread];
      thread.id = t.thread;
      thread.address = t.address;
      thread.last = t.body;
      thread.atMs = t.atMs;
      if (m_unread.contains(t.id) && !t.sent) ++thread.unread;
    }
    m_threads.clear();
    m_threads.reserve(byId.size());
    for (auto& [id, thread] : byId) {
      thread.name = nameFor(thread.address);
      m_threads.push_back(std::move(thread));
    }
    std::ranges::sort(m_threads, [](const Thread& a, const Thread& b) { return std::tie(b.atMs, b.id) < std::tie(a.atMs, a.id); });
  }

  std::vector<ConversationRow> Messages::conversation(std::uint64_t thread, std::int64_t nowMs) const {
    using K = ConversationRow::Kind;
    std::vector<ConversationRow> rows;
    std::string day;
    const auto add = [&](ConversationRow row) {
      const std::string label = dayLabel(row.atMs, nowMs);
      if (label != day) {
        if (!rows.empty()) rows.back().runEnd = rows.back().kind != K::Day;
        day = label;
        rows.push_back({.kind = K::Day, .text = label, .atMs = row.atMs, .key = std::hash<std::string>{}(label)});
      } else if (!rows.empty() && (rows.back().kind == K::In) != (row.kind == K::In)) {
        rows.back().runEnd = true;
      }
      rows.push_back(std::move(row));
    };
    for (const Text& t : m_texts) {
      if (t.thread == thread) add({.kind = t.sent ? K::Out : K::In, .text = t.body, .atMs = t.atMs, .key = t.id});
    }
    for (const Outgoing& out : m_outgoing) {
      if (out.thread != thread) continue;
      add({.kind = out.state == Outgoing::State::Failed ? K::Failed : K::Sending, .text = out.body, .atMs = out.atMs,
           .key = ~out.serial, .serial = out.serial});
    }
    if (!rows.empty()) rows.back().runEnd = true;
    return rows;
  }

} // namespace linkapp
