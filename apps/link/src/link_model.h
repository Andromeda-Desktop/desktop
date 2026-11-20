#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Link desktop app state with no UI or D-Bus in it, so tests drive it directly.
namespace linkapp {

  // One paired phone as Link1 Devices, DeviceInfo, DeviceStatus, DeviceControls and Paused report it.
  struct Phone {
    std::string id;
    std::string name;
    bool connected = false;
    std::string transport;     // wifi or bluetooth while connected
    int battery = -1;          // percent, -1 unknown
    bool charging = false;
    std::int64_t lastSeen = 0; // unix seconds of its last session
    int locked = -1;           // 1, 0, or -1 when the phone does not say
    int dnd = -1;              // same values as locked
    bool paused = false;
  };

  struct Presence {
    enum class Kind : std::uint8_t { NoDaemon, NotPaired, Pairing, Connected, Locked, Away };
    Kind kind = Kind::NoDaemon;
    Phone phone; // empty for NoDaemon, NotPaired and Pairing
  };

  // The one place presence comes from: a pairing in progress wins, then the first connected phone, then the first paired.
  [[nodiscard]] Presence presenceFrom(bool daemon, bool pairing, const std::vector<Phone>& phones);

  enum class Transfer : std::uint8_t { Offered, Running, Done, Failed };

  // Offered -> Running -> Done | Failed; Offered -> Failed; Failed -> Running (Try Again). Anything else is rejected.
  [[nodiscard]] bool transferMayMove(Transfer from, Transfer to);

  struct FeedAction {
    std::string id;
    std::string label;
    bool reply = false; // takes text
  };

  // One row of the Waiting page.
  struct FeedItem {
    enum class Kind : std::uint8_t { Text, Notification, MissedCall, Offer };
    Kind kind = Kind::Notification;
    std::string key; // unique: kind prefix + Link1 id
    std::string deviceId;
    std::string who;
    std::string body;
    std::int64_t atMs = 0;
    std::vector<FeedAction> actions;
    // Offer only.
    std::vector<std::pair<std::string, std::uint64_t>> files;
    Transfer transfer = Transfer::Offered;
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
  };

  // The Waiting feed: newest first, one item per key, at most kMax items (the oldest leave first).
  class Feed {
  public:
    static constexpr std::size_t kMax = 200;
    // Items newer than this belong to "Now"; older ones to "Earlier today".
    static constexpr std::int64_t kNowMs = 60LL * 60 * 1000;

    void upsert(FeedItem item);
    bool remove(const std::string& key);
    // Moves an offer's transfer state; false (and no change) for an unknown key or a rejected move.
    bool moveTransfer(const std::string& key, Transfer to, std::uint64_t bytes = 0, std::uint64_t total = 0);
    void clear() { m_items.clear(); }

    [[nodiscard]] const std::vector<FeedItem>& items() const { return m_items; }
    [[nodiscard]] std::optional<std::size_t> find(const std::string& key) const;
    // Index of the first "Earlier today" item at time nowMs, or items().size() when every item is in "Now".
    [[nodiscard]] std::size_t earlierStart(std::int64_t nowMs) const;

  private:
    std::vector<FeedItem> m_items;
  };

} // namespace linkapp
