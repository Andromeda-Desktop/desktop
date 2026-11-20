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

    [[nodiscard]] Presence presence() const { return presenceFrom(m_daemon, m_phones); }
    [[nodiscard]] const Pair& pair() const { return m_pair; }
    // Opens a pairing window (StartPairing) when this app has none open; the daemon closes it after 120 s.
    void startPairing();
    [[nodiscard]] const Feed& feed() const { return m_feed; }
    // The texts of the phone presence() shows.
    [[nodiscard]] const Messages& messages() const { return m_messages; }
    // The call log of the phone presence() shows, newest first, and whether the phone gives it.
    [[nodiscard]] const std::vector<CallEntry>& calls() const { return m_calls; }
    [[nodiscard]] CallLog callLog() const { return m_callLog; }
    [[nodiscard]] const Activity& activity() const { return m_activity; }
    [[nodiscard]] const Photos& photos() const { return m_photos; }
    [[nodiscard]] const Files& files() const { return m_files; }
    // Lists `path` on the phone presence() shows ("/" for its roots).
    void openFolder(const std::string& path);
    // Copies the file `name` of the open folder into the downloads folder with ReadFile, 1 MiB at a time.
    void saveFile(const std::string& name);
    // RecentPhotos of the phone presence() shows; ignored while a load runs.
    void loadPhotos();
    void togglePhoto(std::size_t index);
    void clearPhotoSelection();
    // SavePhoto for each selected photo into the downloads folder.
    void savePhotos();
    // The PNG thumbnail of a listed photo, or null; the first ask fetches it and calls the thumbnail callback.
    [[nodiscard]] const std::vector<std::uint8_t>* thumbnail(const std::string& path);
    void setOnThumbnail(std::function<void()> ready) { m_thumbnailReady = std::move(ready); }
    [[nodiscard]] const PhoneSettings& phoneSettings() const { return m_phoneSettings; }
    [[nodiscard]] const DesktopSettings& desktopSettings() const { return m_desktopSettings; }
    void setGrant(const std::string& deviceId, const std::string& feature, bool granted);
    void setAutoAccept(const std::string& deviceId, bool on);
    void setNotificationsWhileLocked(bool on);
    void setNearbyVisible(bool on);
    void unpair(const std::string& deviceId);
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
    void acceptTransfer(const std::string& id);
    void declineTransfer(const std::string& id);
    void cancelTransfer(const std::string& id);
    void clearFinished();
    // Opens the file manager on these files (org.freedesktop.FileManager1.ShowItems).
    void showInFolder(const std::vector<std::string>& paths);
    void connect(const std::string& deviceId);
    void ring(const std::string& deviceId);
    void dial(const std::string& deviceId, const std::string& number);
    void setDnd(const std::string& deviceId, bool on);

  private:
    struct Download;
    void readChunk(const std::shared_ptr<Download>& download);
    void refresh();
    void refreshNearby();
    void send(std::uint64_t serial);
    void changed();

    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_proxy;
    std::unique_ptr<sdbus::IProxy> m_fileManager;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void()> m_changed;
    std::uint64_t m_sent = 0;    // serial of the newest refresh asked
    std::uint64_t m_applied = 0; // serial of the newest refresh applied
    bool m_daemon = false;
    bool m_pairing = false; // Link1 Pairing: a window is open, ours or another client's
    Pair m_pair;
    std::vector<Phone> m_phones;
    Feed m_feed;
    Messages m_messages;
    Activity m_activity;
    Photos m_photos;
    Files m_files;
    std::string m_photosDevice;
    std::map<std::string, std::vector<std::uint8_t>> m_thumbs; // path -> PNG; empty while in flight
    std::function<void()> m_thumbnailReady;
    PhoneSettings m_phoneSettings;
    DesktopSettings m_desktopSettings;
    std::uint64_t m_nearbySent = 0;    // serials of the Quick Share GetAll, as m_sent / m_applied
    std::uint64_t m_nearbyApplied = 0;
    std::vector<CallEntry> m_calls;
    CallLog m_callLog = CallLog::Ready;
    std::set<std::string> m_dismissed; // missed calls the user cleared; Calls keeps reporting them
  };

} // namespace linkapp
