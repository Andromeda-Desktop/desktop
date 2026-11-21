#pragma once

#include "link_model.h"
#include "mirror_decoder.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <set>
#include <filesystem>
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

    [[nodiscard]] Presence presence() const { return presenceFrom(m_daemon, m_phones, m_selected); }
    [[nodiscard]] const std::vector<Phone>& phones() const { return m_phones; }
    // Shows this phone from now on, also after a restart; an id that is not paired keeps the default choice.
    void select(const std::string& deviceId);
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
    [[nodiscard]] const Mirror& mirror() const { return m_mirror; }
    // OpenMirror on the phone presence() shows; the phone's user allows it there.
    void startMirror();
    void stopMirror();
    // MirrorInput without args: back, home, recents, screen-off.
    void mirrorAction(const std::string& action);
    // A key press and release (Android key code).
    void mirrorKey(std::int32_t code);
    // The mirrored screen's largest corner radius in 1/10000 of the video width; 0 until the phone says.
    [[nodiscard]] std::uint16_t mirrorCorner() const { return m_mirrorCorner; }
    // touch-down, touch-move or touch-up at x, y in 1/10000 of the video.
    void mirrorTouch(const std::string& action, std::int32_t x, std::int32_t y);
    void setMirrorTarget(int width, int height);
    [[nodiscard]] int mirrorFps() const { return m_decoder ? m_decoder->fps() : 0; }
    void setOnMirrorFrame(std::function<void(MirrorDecoder::Frame)> frame) { m_mirrorFrame = std::move(frame); }
    // The UI could not import a dmabuf frame: the decoder sends RGBA instead.
    void refuseMirrorDmabuf() {
      if (m_decoder) m_decoder->refuseDmabuf();
    }
    // Lists `path` on the phone presence() shows ("/" for its roots).
    void openFolder(const std::string& path);
    // Copies the file `name` of the open folder into the downloads folder with ReadFile, 1 MiB at a time.
    void saveFile(const std::string& name);
    // The first PhotosPage of the phone presence() shows; ignored while a first page loads.
    void loadPhotos();
    // The next PhotosPage when the last answer left more on the phone; ignored while a page loads.
    void loadMorePhotos();
    // Shows only photos, only videos, or both, from the first page.
    void setPhotoMedia(Photos::Media media);
    void togglePhoto(std::size_t index);
    void clearPhotoSelection();
    // SavePhoto for each selected photo into the downloads folder.
    void savePhotos();
    // Saves one listed photo into folder; done gets the saved path, empty on failure.
    void savePhotoTo(const std::string& path, const std::string& folder, std::function<void(const std::string&)> done);
    // The PNG thumbnail of a listed photo, or null; the first ask fetches it and calls the thumbnail callback.
    [[nodiscard]] const std::vector<std::uint8_t>* thumbnail(const std::string& path);
    void setOnThumbnail(std::function<void()> ready) { m_thumbnailReady = std::move(ready); }
    // Marks every loaded photo as seen, so Waiting drops "N new photos" until a newer one loads.
    void markPhotosSeen();
    [[nodiscard]] const PhoneSettings& phoneSettings() const { return m_phoneSettings; }
    [[nodiscard]] const DesktopSettings& desktopSettings() const { return m_desktopSettings; }
    void setGrant(const std::string& deviceId, const std::string& feature, bool granted);
    // Replaces Link1 Commands; `done` gets the error text, or nothing on success.
    void setCommands(const std::vector<Command>& commands, std::function<void(std::optional<std::string>)> done);
    void setAutoAccept(const std::string& deviceId, bool on);
    [[nodiscard]] const std::optional<HotspotAsk>& hotspotAsk() const { return m_hotspotAsk; }
    // Answers the open ask; `always` also grants "nearby", so the next ask joins without one.
    void answerHotspot(bool allow, bool always = false);
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
    // Sends text to the phone's clipboard; a lone http(s) URL goes as a link.
    void shareText(const std::string& deviceId, const std::string& text);
    // Texts any number from the phone presence() shows; its thread shows once the phone lists the text.
    void sendTo(const std::string& number, const std::string& body);
    void sendFiles(const std::string& deviceId, const std::vector<std::filesystem::path>& paths);

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
    std::unique_ptr<sdbus::IProxy> m_busWatch;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void()> m_changed;
    std::uint64_t m_sent = 0;    // serial of the newest refresh asked
    std::uint64_t m_applied = 0; // serial of the newest refresh applied
    bool m_daemon = false;
    bool m_pairing = false; // Link1 Pairing: a window is open, ours or another client's
    Pair m_pair;
    std::vector<Phone> m_phones;
    std::string m_selected;
    Feed m_feed;
    Messages m_messages;
    Activity m_activity;
    Photos m_photos;
    Files m_files;
    Mirror m_mirror;
    std::uint16_t m_mirrorCorner = 0;
    std::string m_mirrorDevice;
    std::unique_ptr<MirrorDecoder> m_decoder;
    std::function<void(MirrorDecoder::Frame)> m_mirrorFrame;
    std::string m_photosDevice;
    void askPhotos(const Photos::Ask& ask);
    void clearThumbs();
    struct Thumb {
      std::vector<std::uint8_t> png; // empty while in flight
      std::list<std::string>::iterator use;
    };
    static constexpr std::size_t kThumbBytes = std::size_t{48} << 20;
    std::map<std::string, Thumb> m_thumbs; // path -> PNG, at most kThumbBytes; the least recently drawn go first
    std::list<std::string> m_thumbUse;     // paths, most recently drawn first
    std::size_t m_thumbBytes = 0;
    std::function<void()> m_thumbnailReady;
    std::int64_t m_photosSeenS = 0; // the newest photo of m_photosDevice the user saw
    PhoneSettings m_phoneSettings;
    std::optional<HotspotAsk> m_hotspotAsk;
    DesktopSettings m_desktopSettings;
    std::uint64_t m_nearbySent = 0;    // serials of the Quick Share GetAll, as m_sent / m_applied
    std::uint64_t m_nearbyApplied = 0;
    std::vector<CallEntry> m_calls;
    CallLog m_callLog = CallLog::Ready;
    std::set<std::string> m_dismissed; // missed calls the user cleared; Calls keeps reporting them
  };

} // namespace linkapp
