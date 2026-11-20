#include "link_model.h"

#include <algorithm>

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

} // namespace linkapp
