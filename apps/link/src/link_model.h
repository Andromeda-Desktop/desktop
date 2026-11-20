#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
    enum class Kind : std::uint8_t { NoDaemon, NotPaired, Connected, Locked, Away };
    Kind kind = Kind::NoDaemon;
    Phone phone; // empty for NoDaemon and NotPaired
  };

  // The one place presence comes from: the first connected phone, else the first paired one.
  [[nodiscard]] Presence presenceFrom(bool daemon, const std::vector<Phone>& phones);

  // This app's pairing window: Idle -> Starting -> Showing -> Idle (finished, or closed by expiry or another client);
  // Starting | Showing -> Failed -> Starting (Try Again). pairStep rejects every other move and leaves the state as is.
  struct Pair {
    enum class Kind : std::uint8_t { Idle, Starting, Showing, Failed };
    Kind kind = Kind::Idle;
    std::string code;  // Showing only
    std::string uri;   // Showing only
    std::string error; // Failed only
  };

  struct PairEvent {
    enum class Kind : std::uint8_t { Start, Started, Failed, Finished, Closed };
    Kind kind = Kind::Start;
    std::string code;  // Started
    std::string uri;   // Started
    std::string error; // Failed
  };

  [[nodiscard]] bool pairStep(Pair& pair, const PairEvent& event);

  enum class Transfer : std::uint8_t { Offered, Running, Done, Failed };

  // Offered -> Running -> Done | Failed; Offered -> Done (a small file may finish before any progress) | Failed;
  // Failed -> Running (Try Again). Anything else is rejected.
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

  // The Settings page: one phone's switches (Link1 Grants and AutoAccept) and this desktop's.
  struct PhoneSettings {
    bool notifications = false; // grant "notifications"
    bool clipboard = false;     // grant "clipboard"
    bool screen = false;        // grant "screen"
    bool autoAccept = false;
  };
  struct DesktopSettings {
    bool notificationsWhileLocked = false;
    bool nearbyVisible = false; // Quick Share Visible
    std::string nearbyName;
    std::size_t commands = 0;
  };

  // One transfer on the Activity page, from the Transfer* signals (Link1 keeps no list).
  struct TransferEntry {
    std::string id;
    std::string deviceId;
    bool incoming = true;
    std::vector<std::pair<std::string, std::uint64_t>> files;
    Transfer state = Transfer::Offered;
    std::string status; // TransferFinished status: done, failed, no-space, too-large, busy, cancelled
    std::vector<std::string> paths; // where an incoming transfer saved its files
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    std::int64_t atMs = 0;     // offered or first seen
    std::int64_t endMs = 0;    // finished
    std::int64_t rateFromMs = 0; // the first progress: the rate counts from here
    std::uint64_t rateFromBytes = 0;
  };

  // The Activity list, newest first. A progress for an unknown id is a transfer this desktop sends (SendFiles from
  // any app); a declined offer leaves the list.
  class Activity {
  public:
    void offered(const std::string& id, const std::string& deviceId,
                 std::vector<std::pair<std::string, std::uint64_t>> files, std::int64_t nowMs);
    // False when the move is rejected (a finished transfer) and nothing changed.
    bool progress(const std::string& id, std::uint64_t bytes, std::uint64_t total, std::int64_t nowMs);
    bool finished(const std::string& id, const std::string& status, std::vector<std::string> paths, std::int64_t nowMs);
    void clearFinished();

    [[nodiscard]] const std::vector<TransferEntry>& items() const { return m_items; }
    [[nodiscard]] std::size_t finishedCount() const;
    [[nodiscard]] std::size_t offeredCount() const;

  private:
    std::vector<TransferEntry> m_items;
  };

  // Bytes per second since the first progress; 0 until a second sample.
  [[nodiscard]] double transferRate(const TransferEntry& entry, std::int64_t nowMs);
  // "2.4 MB", "512 KB", "1.2 GB".
  [[nodiscard]] std::string byteSize(std::uint64_t bytes);

  // One SMS as Link1 Texts reports it.
  struct Text {
    std::uint64_t id = 0;
    std::uint64_t thread = 0;
    std::string address;
    std::string body;
    std::int64_t atMs = 0;
    bool sent = false;
  };

  struct Contact {
    std::string name;
    std::vector<std::string> numbers;
  };

  struct Thread {
    std::uint64_t id = 0;
    std::string address; // the other side's number
    std::string name;    // the contact name, else the address
    std::string last;    // the newest body
    std::int64_t atMs = 0;
    int unread = 0;
  };

  // A text the user sent that Texts does not hold yet: Sending -> (in Texts: gone) | Failed; Failed -> Sending.
  struct Outgoing {
    enum class State : std::uint8_t { Sending, Failed };
    std::uint64_t serial = 0;
    std::uint64_t thread = 0;
    std::string body;
    std::int64_t atMs = 0;
    State state = State::Sending;
  };

  // One row of a conversation, oldest first.
  struct ConversationRow {
    enum class Kind : std::uint8_t { Day, In, Out, Sending, Failed };
    Kind kind = Kind::Day;
    std::string text; // the day label or the body
    std::int64_t atMs = 0;
    std::uint64_t key = 0;   // stable per row: text id, outgoing serial, or day
    bool runEnd = false; // the last bubble before the other side, a day line or the end: its time shows below
    std::uint64_t serial = 0; // Sending and Failed: the outgoing serial
  };

  struct CallEntry {
    enum class Kind : std::uint8_t { Incoming, Outgoing, Missed };
    std::string number;
    std::string name; // the phone's contact name, or empty
    std::int64_t atMs = 0;
    std::uint32_t seconds = 0;
    Kind kind = Kind::Incoming;
  };

  // Whether the phone gives its call log (Link1 CallsRefused): "" Ready, not-allowed Off, denied Denied, else Failed.
  enum class CallLog : std::uint8_t { Ready, Off, Denied, Failed };
  [[nodiscard]] CallLog callLogFrom(const std::string& refused);

  // "0:41", "12:48", "1:02:03".
  [[nodiscard]] std::string duration(std::uint32_t seconds);

  // "Today", "Yesterday", a weekday within the last week, else "12 Mar" (and the year when it is not this year).
  [[nodiscard]] std::string dayLabel(std::int64_t atMs, std::int64_t nowMs);

  // "Last seen 12 min ago" from unix seconds; "Not connected" when the phone was never seen.
  [[nodiscard]] std::string lastSeenText(std::int64_t lastSeenS, std::int64_t nowS);

  // Match key of a phone number: its last 9 digits, so "+49 151 2345 6789" and "0151 23456789" meet; a name-like
  // sender (no digits) keys as itself.
  [[nodiscard]] std::string numberKey(const std::string& address);

  // The Messages page: threads from the newest texts Link1 keeps, unread from TextReceived since start (Link1 has no
  // read state), and texts the user sent until Texts holds them.
  class Messages {
  public:
    enum class Load : std::uint8_t { Loading, Ready };

    // The phone's texts, any order. Loading -> Ready; drops each outgoing text the new list holds.
    void setTexts(std::vector<Text> texts);
    void setContacts(const std::vector<Contact>& contacts);
    // A new text arrived: unread until its thread is on screen.
    void received(std::uint64_t textId);
    // The thread on screen (nullopt: none); its texts count as read now and while it stays on screen.
    void view(std::optional<std::uint64_t> thread);
    // Returns the outgoing serial; 0 (and no change) for an unknown thread or an empty body.
    std::uint64_t send(std::uint64_t thread, std::string body, std::int64_t atMs);
    // Sending -> Failed; false for an unknown serial or another state.
    bool sendFailed(std::uint64_t serial);
    // Failed -> Sending; false otherwise.
    bool retry(std::uint64_t serial);

    [[nodiscard]] Load load() const { return m_load; }
    // Newest first.
    [[nodiscard]] const std::vector<Thread>& threads() const { return m_threads; }
    [[nodiscard]] const Thread* thread(std::uint64_t id) const;
    [[nodiscard]] const Outgoing* outgoing(std::uint64_t serial) const;
    [[nodiscard]] int unread() const;
    // The contact name for a number, else the number.
    [[nodiscard]] std::string nameFor(const std::string& address) const;
    // What to dial for "Name or number": the digits, + * # of a number, else the first number of the first contact
    // whose name starts with the query (any case); empty when neither.
    [[nodiscard]] std::string numberFor(const std::string& query) const;
    [[nodiscard]] std::vector<ConversationRow> conversation(std::uint64_t thread, std::int64_t nowMs) const;

  private:
    void rebuild();
    void readViewed();

    Load m_load = Load::Loading;
    std::vector<Text> m_texts;
    std::map<std::string, std::string> m_names; // numberKey -> contact name
    std::vector<Contact> m_contacts;
    std::set<std::uint64_t> m_unread;           // text ids
    std::optional<std::uint64_t> m_viewing;
    std::vector<Outgoing> m_outgoing;
    std::uint64_t m_serial = 0;
    std::vector<Thread> m_threads;
  };

} // namespace linkapp
