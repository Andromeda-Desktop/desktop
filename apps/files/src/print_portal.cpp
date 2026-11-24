#include "print_portal.h"

#include "core/blocking_pool.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "core/log.h"
#include "printing/cups_printers.h"
#include "dbus/session_bus.h"
#include "state_file.h"

#include <glib.h>
#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <fcntl.h>
#include <format>
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
      o.margins = static_cast<Margins>(std::clamp(integer(g, "margins", 0), 0, 3));
      o.reverseOrder = integer(g, "reverse", 0) != 0;
      o.booklet = integer(g, "booklet", 0) != 0;
      // "key=value;key=value": the printer's own IPP options.
      const std::string own = str(g, "printer-options");
      for (std::string_view rest = own; !rest.empty();) {
        const auto semi = rest.find(';');
        const std::string_view kv = rest.substr(0, semi);
        if (const auto eq = kv.find('='); eq != std::string_view::npos && eq > 0)
          o.printerOptions.emplace_back(std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1)));
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
      }
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
    g_key_file_set_integer(kf, g.c_str(), "margins", static_cast<int>(o.margins));
    g_key_file_set_integer(kf, g.c_str(), "reverse", o.reverseOrder ? 1 : 0);
    g_key_file_set_integer(kf, g.c_str(), "booklet", o.booklet ? 1 : 0);
    std::string own;
    for (const auto& [k, v] : o.printerOptions)
      own += (own.empty() ? "" : ";") + k + '=' + v;
    g_key_file_set_string(kf, g.c_str(), "printer-options", own.c_str());
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
    : m_bus(bus), m_handlers(std::move(handlers)), m_stateFile(std::move(stateFile)), m_jobs(bus) {
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

void PrintPortal::print(std::uint64_t id, const PrintOptions& options, const std::string& printerLabel, int pages) {
  if (!apply(id, PrintEvent::UserPrint))
    return;
  Pending& p = *m_pending.at(id);
  saveRemembered(m_stateFile, p.appId, options);
  BlockingPool::instance().run([this, id, fd = ::dup(p.fd), options, printerLabel, pages, title = p.title,
               alive = std::weak_ptr<bool>(m_alive)] {
    auto outcome = submitJob(title, fd, options);
    ::close(fd);
    DeferredCall::callLater([this, id, outcome = std::move(outcome), options, printerLabel, pages, title, alive] {
      if (alive.expired())
        return;
      if (const auto* e = std::get_if<JobError>(&outcome)) {
        kLog.warn("print job failed: {}", e->message);
        m_jobs.failed(i18n::tr("files.printing.failed-title", "title", title),
                      i18n::tr("files.printing.job-error", "printer", printerLabel, "reason", e->message));
        apply(id, PrintEvent::JobFailed);
        return;
      }
      m_jobs.track(std::get<int>(outcome), options.printer, printerLabel, title, pages);
      apply(id, PrintEvent::JobSent);
    });
  });
}

void PrintPortal::savePdf(std::uint64_t id, const std::filesystem::path& path) {
  if (!apply(id, PrintEvent::UserPrint))
    return;
  const Pending& p = *m_pending.at(id);
  BlockingPool::instance().run([this, id, fd = ::dup(p.fd), path, title = p.title, alive = std::weak_ptr<bool>(m_alive)] {
    std::error_code ec;
    const std::filesystem::path tmp = path.string() + ".part";
    bool ok = false;
    if (const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); out >= 0) {
      char buf[65536];
      ok = true;
      for (off_t offset = 0;;) {
        const ssize_t n = ::pread(fd, buf, sizeof buf, offset);
        if (n <= 0) {
          ok = n == 0;
          break;
        }
        if (::write(out, buf, static_cast<std::size_t>(n)) != n) {
          ok = false;
          break;
        }
        offset += n;
      }
      ok = ::close(out) == 0 && ok;
    }
    ::close(fd);
    if (ok)
      std::filesystem::rename(tmp, path, ec);
    if (!ok || ec)
      std::filesystem::remove(tmp, ec);
    const bool saved = ok && !ec;
    DeferredCall::callLater([this, id, saved, path, title, alive] {
      if (alive.expired())
        return;
      if (!saved) {
        kLog.warn("save as PDF failed: {}", path.string());
        m_jobs.failed(i18n::tr("files.printing.save-failed", "title", title),
                      i18n::tr("files.printing.save-failed-detail", "name", path.filename().string()));
      }
      apply(id, saved ? PrintEvent::JobSent : PrintEvent::JobFailed);
    });
  });
}
