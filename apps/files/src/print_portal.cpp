#include "print_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "cups_printers.h"
#include "dbus/session_bus.h"
#include "state_file.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  constexpr Logger kLog("print-portal");
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Print";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
  using Options = std::map<std::string, sdbus::Variant>;

  // Group "<app>" holds the last printer; group "<app>/<printer>" the options for that printer.
  PrintOptions loadRemembered(const std::filesystem::path& file, const std::string& appId) {
    PrintOptions o;
    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_NONE, nullptr)) {
      const auto str = [&](const std::string& group, const char* key) {
        gchar* v = g_key_file_get_string(kf, group.c_str(), key, nullptr);
        std::string out = v ? v : "";
        g_free(v);
        return out;
      };
      const auto integer = [&](const std::string& group, const char* key, int fallback) {
        GError* e = nullptr;
        const int v = g_key_file_get_integer(kf, group.c_str(), key, &e);
        if (e) {
          g_error_free(e);
          return fallback;
        }
        return v;
      };
      o.printer = str(appId, "printer");
      const std::string g = appId + "/" + o.printer;
      o.copies = std::max(1, integer(g, "copies", 1));
      o.collate = integer(g, "collate", 1) != 0;
      o.duplex = static_cast<Duplex>(std::clamp(integer(g, "duplex", 0), 0, 2));
      o.colour = integer(g, "colour", 1) != 0;
      o.media = str(g, "media");
      o.numberUp = std::clamp(integer(g, "number-up", 1), 1, 16);
      o.orientation = static_cast<Orientation>(std::clamp(integer(g, "orientation", 0), 0, 2));
      o.scaling = static_cast<Scaling>(std::clamp(integer(g, "scaling", 0), 0, 2));
      o.scalePercent = std::clamp(integer(g, "scale", 100), 10, 400);
    }
    g_key_file_free(kf);
    return o;
  }

  void saveRemembered(const std::filesystem::path& file, const std::string& appId, const PrintOptions& o) {
    GKeyFile* kf = g_key_file_new();
    g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    const std::string g = appId + "/" + o.printer;
    g_key_file_set_string(kf, appId.c_str(), "printer", o.printer.c_str());
    g_key_file_set_integer(kf, g.c_str(), "copies", o.copies);
    g_key_file_set_integer(kf, g.c_str(), "collate", o.collate ? 1 : 0);
    g_key_file_set_integer(kf, g.c_str(), "duplex", static_cast<int>(o.duplex));
    g_key_file_set_integer(kf, g.c_str(), "colour", o.colour ? 1 : 0);
    g_key_file_set_string(kf, g.c_str(), "media", o.media.c_str());
    g_key_file_set_integer(kf, g.c_str(), "number-up", o.numberUp);
    g_key_file_set_integer(kf, g.c_str(), "orientation", static_cast<int>(o.orientation));
    g_key_file_set_integer(kf, g.c_str(), "scaling", static_cast<int>(o.scaling));
    g_key_file_set_integer(kf, g.c_str(), "scale", o.scalePercent);
    gsize size = 0;
    gchar* data = g_key_file_to_data(kf, &size, nullptr);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!writeStateFile(file, std::string_view(data, size)))
      kLog.warn("could not save print settings to {}", file.string());
    g_free(data);
    g_key_file_free(kf);
  }

  // The app renders every page once at 100 %: copies, ranges and pages per sheet are applied by CUPS from the sheet.
  Options neutralSettings(const PrintOptions& o) {
    Options s;
    s["n-copies"] = sdbus::Variant(std::string("1"));
    s["print-pages"] = sdbus::Variant(std::string("all"));
    s["number-up"] = sdbus::Variant(std::string("1"));
    s["scale"] = sdbus::Variant(std::string("100"));
    s["use-color"] = sdbus::Variant(std::string(o.colour ? "true" : "false"));
    if (!o.printer.empty())
      s["printer"] = sdbus::Variant(o.printer);
    return s;
  }

  Options pageSetup(const PrintOptions& o, const Options& fromApp) {
    if (o.media.empty())
      return fromApp;
    const auto size = mediaSizeMm(o.media);
    if (!size)
      return fromApp;
    Options p = fromApp;
    p["PPDName"] = sdbus::Variant(std::string());
    p["Name"] = sdbus::Variant(o.media);
    p["DisplayName"] = sdbus::Variant(o.media);
    p["Width"] = sdbus::Variant(size->first);
    p["Height"] = sdbus::Variant(size->second);
    if (o.orientation != Orientation::Auto)
      p["Orientation"] = sdbus::Variant(std::string(o.orientation == Orientation::Landscape ? "landscape" : "portrait"));
    return p;
  }
} // namespace

struct PrintPortal::Pending {
  sdbus::Result<std::uint32_t, Options> result;
  std::unique_ptr<sdbus::IObject> requestObject;
  PrintStage stage = PrintStage::SheetOpen;
  std::string appId;
  std::string title;
  int fd = -1;

  explicit Pending(sdbus::Result<std::uint32_t, Options>&& r) : result(std::move(r)) {}
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  ~Pending() {
    if (fd >= 0)
      ::close(fd);
  }
};

PrintPortal::PrintPortal(SessionBus& bus, Handlers handlers, std::filesystem::path stateFile)
    : m_bus(bus), m_handlers(std::move(handlers)), m_stateFile(std::move(stateFile)) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("PreparePrint")
              .withInputParamNames("handle", "app_id", "parent_window", "title", "settings", "page_setup", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](sdbus::ObjectPath /*handle*/, std::string appId, std::string /*parent*/,
                                    std::string /*title*/, Options /*settings*/, Options pageSetupIn,
                                    Options /*options*/) {
                const PrintOptions o = loadRemembered(m_stateFile, appId);
                Options results;
                results["settings"] = sdbus::Variant(neutralSettings(o));
                results["page-setup"] = sdbus::Variant(pageSetup(o, pageSetupIn));
                results["token"] = sdbus::Variant(m_nextToken++);
                kLog.info("prepare {}: printer {}", appId.empty() ? "?" : appId, o.printer.empty() ? "-" : o.printer);
                return std::tuple{std::uint32_t{0}, results};
              }),
          sdbus::registerMethod("Print")
              .withInputParamNames("handle", "app_id", "parent_window", "title", "fd", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](sdbus::Result<std::uint32_t, Options>&& result, sdbus::ObjectPath handle,
                                    std::string appId, std::string parent, std::string title, sdbus::UnixFd fd,
                                    Options /*options*/) {
                const std::uint64_t id = m_nextId++;
                auto pending = std::make_unique<Pending>(std::move(result));
                pending->appId = appId;
                pending->title = title;
                pending->fd = ::dup(fd.get());
                pending->requestObject = sdbus::createObject(m_bus.connection(), handle);
                pending->requestObject
                    ->addVTable(sdbus::registerMethod("Close").implementedAs([this, id] {
                      const bool sheetOpen = stageOf(id) == PrintStage::SheetOpen;
                      if (apply(id, PrintEvent::CallerClose) && sheetOpen && m_handlers.closed)
                        m_handlers.closed(id);
                    }))
                    .forInterface(kRequestInterface);
                const PrintRequest request{.appId = appId, .parentWindow = std::move(parent),
                                           .title = std::move(title), .fd = pending->fd,
                                           .remembered = loadRemembered(m_stateFile, appId)};
                m_pending.emplace(id, std::move(pending));
                kLog.info("print from {}: \"{}\"", appId.empty() ? "?" : appId, request.title);
                if (m_handlers.open)
                  m_handlers.open(id, request);
              }))
      .forInterface(kInterface);
}

PrintPortal::~PrintPortal() { *m_alive = false; }

std::optional<PrintStage> PrintPortal::stageOf(std::uint64_t id) const {
  const auto it = m_pending.find(id);
  return it == m_pending.end() ? std::nullopt : std::optional{it->second->stage};
}

bool PrintPortal::apply(std::uint64_t id, PrintEvent event) {
  const auto it = m_pending.find(id);
  if (it == m_pending.end())
    return false;
  const auto t = printTransition(it->second->stage, event);
  if (!t)
    return false;
  it->second->stage = t->next;
  if (t->reply)
    it->second->result.returnResults(*t->reply, Options{});
  if (t->next == PrintStage::Ended) {
    // A Close callback belongs to the Request object: drop the request after the callback returns.
    DeferredCall::callLater([this, id, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired())
        m_pending.erase(id);
    });
  }
  return true;
}

void PrintPortal::cancel(std::uint64_t id) { apply(id, PrintEvent::UserCancel); }

void PrintPortal::print(std::uint64_t id, const PrintOptions& options,
                        std::function<void(std::optional<std::string> error)> done) {
  if (!apply(id, PrintEvent::UserPrint))
    return;
  Pending& p = *m_pending.at(id);
  saveRemembered(m_stateFile, p.appId, options);
  std::thread([this, id, fd = ::dup(p.fd), options, title = p.title, done = std::move(done),
               alive = std::weak_ptr<bool>(m_alive)] {
    auto outcome = submitJob(title, fd, options);
    ::close(fd);
    std::optional<std::string> error;
    if (const auto* e = std::get_if<JobError>(&outcome))
      error = e->message;
    DeferredCall::callLater([this, id, error, done, alive] {
      if (alive.expired())
        return;
      if (error)
        kLog.warn("print job failed: {}", *error);
      apply(id, error ? PrintEvent::JobFailed : PrintEvent::JobSent);
      if (done)
        done(error);
    });
  }).detach();
}
