#pragma once

#include "link_model.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

namespace linkapp {

  // org.andromeda.Link1 for the app: phones and the Waiting feed, kept from properties and signals. Every call is
  // async and replies land on the app loop; a reply older than one already applied is dropped.
  class LinkClient {
  public:
    explicit LinkClient(SessionBus& bus);
    ~LinkClient();
    LinkClient(const LinkClient&) = delete;
    LinkClient& operator=(const LinkClient&) = delete;

    void setOnChanged(std::function<void()> changed) { m_changed = std::move(changed); }

    [[nodiscard]] Presence presence() const { return presenceFrom(m_daemon, m_pairing, m_phones); }
    [[nodiscard]] const Feed& feed() const { return m_feed; }
    // The texts of the phone presence() shows.
    [[nodiscard]] const Messages& messages() const { return m_messages; }
    // The call log of the phone presence() shows, newest first, and whether the phone gives it.
    [[nodiscard]] const std::vector<CallEntry>& calls() const { return m_calls; }
    [[nodiscard]] CallLog callLog() const { return m_callLog; }
    void viewThread(std::optional<std::uint64_t> thread);
    // Sends `body` to `thread`'s address; it shows as sending until Texts holds it, failed on an error.
    void sendText(std::uint64_t thread, const std::string& body);
    void retryText(std::uint64_t serial);

    void reply(const FeedItem& item, const std::string& text);
    void action(const FeedItem& item, const std::string& actionId);
    void dismiss(const FeedItem& item);
    void accept(const FeedItem& offer);
    void decline(const FeedItem& offer);
    void clearAll();
    void connect(const std::string& deviceId);
    void ring(const std::string& deviceId);
    void dial(const std::string& deviceId, const std::string& number);
    void setDnd(const std::string& deviceId, bool on);

  private:
    void refresh();
    void send(std::uint64_t serial);
    void changed();

    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_proxy;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void()> m_changed;
    std::uint64_t m_sent = 0;    // serial of the newest refresh asked
    std::uint64_t m_applied = 0; // serial of the newest refresh applied
    bool m_daemon = false;
    bool m_pairing = false;
    std::vector<Phone> m_phones;
    Feed m_feed;
    Messages m_messages;
    std::vector<CallEntry> m_calls;
    CallLog m_callLog = CallLog::Ready;
    std::set<std::string> m_dismissed; // missed calls the user cleared; Calls keeps reporting them
  };

} // namespace linkapp
