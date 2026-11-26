#include "system/location_service.h"

#include "cli/schema_msg.h"
#include "config/atomic_file.h"
#include "config/config_service.h"
#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "net/uri.h"
#include "protocols/xdg_dirs.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <stdexcept>
#include <system_error>

namespace {

  constexpr Logger kLog("location");

  using Clock = std::chrono::system_clock;

  // IP-derived position can change while the shell runs (travel/VPN); re-resolve periodically.
  constexpr auto kRefreshInterval = std::chrono::hours(6);
  constexpr auto kRetryInterval = std::chrono::minutes(5);
  // Address geocoding is stable once resolved; idle until the address changes.
  constexpr auto kIdleInterval = std::chrono::hours(24 * 30);

  const sdbus::ServiceName kGeoclueName{"org.freedesktop.GeoClue2"};
  const sdbus::ObjectPath kGeoclueManagerPath{"/org/freedesktop/GeoClue2/Manager"};
  constexpr auto kGeoclueManager = "org.freedesktop.GeoClue2.Manager";
  constexpr auto kGeoclueClient = "org.freedesktop.GeoClue2.Client";
  constexpr auto kGeoclueLocation = "org.freedesktop.GeoClue2.Location";
  constexpr auto kProperties = "org.freedesktop.DBus.Properties";
  // GeoClue lets a client through by the desktop file it names; weather and night light need no more than a city.
  constexpr auto kGeoclueDesktopId = "dev.noctalia.Noctalia";
  constexpr std::uint32_t kGeoclueAccuracyCity = 4;

  double readNumber(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      throw std::runtime_error(std::string("missing numeric key: ") + key);
    }
    return it->get<double>();
  }

  std::string readString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  bool readBool(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_boolean()) {
      return false;
    }
    return it->get<bool>();
  }

} // namespace

LocationService::LocationService(ConfigService& configService, HttpClient& httpClient)
    : m_configService(configService), m_httpClient(httpClient) {}

void LocationService::initialize() {
  m_config = m_configService.config().location;
  m_configService.addReloadCallback([this] { onConfigReload(); });
  loadCache();
  if (m_systemBus != nullptr) {
    // A GeoClue restart drops our client: forget it, so the next refresh asks the new instance.
    m_geoclueOwner = NameOwnerWatch::changes(
        m_systemBus->connection(), std::string{kGeoclueName},
        [this](const std::string& /*name*/, const std::string& oldOwner, const std::string& /*newOwner*/) {
          if (oldOwner.empty() || m_geoclueClient == nullptr) {
            return;
          }
          kLog.info("GeoClue left the bus");
          m_geoclueLocation.reset();
          m_geoclueClient.reset();
          if (m_requestKind == RequestKind::Geolocate) {
            m_requestKind = RequestKind::None;
          }
          scheduleRetryAfterFailure();
        });
  }
  if (networkResolutionConfigured() && !m_resolved) {
    requestRefresh();
  }
}

void LocationService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

bool LocationService::networkResolutionConfigured() const noexcept {
  return m_config.autoLocate || !m_config.address.empty();
}

bool LocationService::resolving() const noexcept { return networkResolutionConfigured() && !m_resolved; }

bool LocationService::hasResolvedLocation() const noexcept {
  return (m_resolved && resolvedCoordinatesValid()) || (!networkResolutionConfigured() && manualCoordinatesValid());
}

std::optional<ResolvedLocation> LocationService::resolvedLocation() const noexcept {
  if (m_resolved && resolvedCoordinatesValid()) {
    return ResolvedLocation{
        .latitude = m_latitude, .longitude = m_longitude, .name = m_name, .sourceLabel = m_sourceLabel
    };
  }
  if (networkResolutionConfigured() || !manualCoordinatesValid()) {
    return std::nullopt;
  }

  const double latitude = *m_config.latitude;
  const double longitude = *m_config.longitude;
  return ResolvedLocation{
      .latitude = latitude,
      .longitude = longitude,
      .name = std::format("{:.4f}, {:.4f}", latitude, longitude),
      .sourceLabel = i18n::tr("location.source.manual"),
  };
}

int LocationService::pollTimeoutMs() const {
  if (m_requestKind != RequestKind::None) {
    return -1;
  }
  if (m_refreshQueued) {
    return 0;
  }
  if (!networkResolutionConfigured()) {
    return -1;
  }
  const auto now = Clock::now();
  if (m_nextRefreshAt <= now) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextRefreshAt - now).count());
}

void LocationService::tick() {
  if (m_requestKind != RequestKind::None) {
    return;
  }
  if (!networkResolutionConfigured()) {
    m_refreshQueued = false;
    if (m_resolved || !m_error.empty()) {
      clearResolved();
      m_error.clear();
      notifyChanged();
    }
    return;
  }

  const auto now = Clock::now();
  if (!m_refreshQueued && now < m_nextRefreshAt) {
    return;
  }
  m_refreshQueued = false;

  if (m_config.autoLocate) {
    startGeolocate();
    return;
  }
  if (!m_resolved || m_resolvedAutoLocate || m_resolvedAddress != m_config.address) {
    startAddressGeocode();
    return;
  }
  m_nextRefreshAt = now + kIdleInterval;
}

void LocationService::onConfigReload() {
  const LocationConfig next = m_configService.config().location;
  if (next == m_config) {
    return;
  }
  const bool resolutionInputsChanged = next.autoLocate != m_config.autoLocate || next.address != m_config.address;
  const bool manualCoordinatesChanged = next.latitude != m_config.latitude || next.longitude != m_config.longitude;
  m_config = next;

  if (resolutionInputsChanged) {
    if (!m_config.autoLocate) {
      stopGeoclue();
    }
    m_error.clear();
    if (networkResolutionConfigured()) {
      requestRefresh();
    } else {
      clearResolved();
    }
    notifyChanged();
  } else if (!networkResolutionConfigured() && manualCoordinatesChanged) {
    notifyChanged();
  }
}

void LocationService::notifyChanged() {
  for (const auto& callback : m_callbacks) {
    callback();
  }
}

void LocationService::requestRefresh() {
  m_refreshQueued = true;
  m_nextRefreshAt = Clock::time_point{};
}

LocationService::~LocationService() { stopGeoclue(); }

void LocationService::startGeoclue() {
  if (m_geoclueClient != nullptr || m_geoclueGetClient.isPending()) {
    return;
  }
  // Never resets m_geoclueManager: this runs inside its reply callback, and sdbus uses the proxy after the callback.
  const auto fail = [this](const std::string& what) {
    kLog.warn("GeoClue unavailable: {}", what);
    m_geoclueClient.reset();
    m_requestKind = RequestKind::None;
    scheduleRetryAfterFailure();
  };
  try {
    // GetClient may start geoclue through D-Bus activation, which takes seconds: never wait for it here.
    if (m_geoclueManager == nullptr) {
      m_geoclueManager = sdbus::createProxy(m_systemBus->connection(), kGeoclueName, kGeoclueManagerPath);
    }
    m_geoclueGetClient = m_geoclueManager->callMethodAsync("GetClient")
        .onInterface(kGeoclueManager)
        .uponReplyInvoke([this, fail](std::optional<sdbus::Error> error, sdbus::ObjectPath clientPath) {
          if (error.has_value()) {
            fail(error->what());
            return;
          }
          try {
            startGeoclueClient(clientPath);
          } catch (const sdbus::Error& e) {
            fail(e.what());
          }
        });
    m_requestKind = RequestKind::Geolocate;
  } catch (const sdbus::Error& e) {
    fail(e.what());
  }
}

// One connection delivers its messages in order, so geoclue sets DesktopId and the accuracy before Start.
void LocationService::startGeoclueClient(const sdbus::ObjectPath& clientPath) {
  m_geoclueClient = sdbus::createProxy(m_systemBus->connection(), kGeoclueName, clientPath);
  const auto onSetReply = [](std::optional<sdbus::Error> error) {
    if (error.has_value()) {
      kLog.warn("configuring the GeoClue client failed: {}", error->what());
    }
  };
  m_geoclueClient->setPropertyAsync("DesktopId")
      .onInterface(kGeoclueClient)
      .toValue(std::string{kGeoclueDesktopId})
      .uponReplyInvoke(onSetReply);
  m_geoclueClient->setPropertyAsync("RequestedAccuracyLevel")
      .onInterface(kGeoclueClient)
      .toValue(kGeoclueAccuracyCity)
      .uponReplyInvoke(onSetReply);
  m_geoclueClient->uponSignal("LocationUpdated")
      .onInterface(kGeoclueClient)
      .call([this](const sdbus::ObjectPath& /*old*/, const sdbus::ObjectPath& location) {
        onGeoclueLocation(location);
      });
  m_geoclueClient->callMethodAsync("Start")
      .onInterface(kGeoclueClient)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("GeoClue refused to locate: {}", error->what());
          m_error = i18n::tr("location.errors.ip-geolocation-failed");
          scheduleRetryAfterFailure();
          notifyChanged();
        }
      });
}

void LocationService::stopGeoclue() {
  m_geoclueManager.reset();
  if (m_geoclueClient == nullptr) {
    return;
  }
  try {
    m_geoclueClient->callMethod("Stop").onInterface(kGeoclueClient).dontExpectReply();
  } catch (const sdbus::Error& e) {
    kLog.debug("stopping GeoClue: {}", e.what());
  }
  m_geoclueLocation.reset();
  m_geoclueClient.reset();
}

void LocationService::onGeoclueLocation(const std::string& path) {
  m_geoclueLocation = sdbus::createProxy(m_systemBus->connection(), kGeoclueName, sdbus::ObjectPath{path});
  m_geoclueLocation->callMethodAsync("GetAll")
      .onInterface(kProperties)
      .withArguments(std::string{kGeoclueLocation})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> properties) {
        m_requestKind = RequestKind::None;
        if (error.has_value()) {
          kLog.warn("reading the GeoClue location failed: {}", error->what());
          return;
        }
        try {
          const double latitude = properties.at("Latitude").get<double>();
          const double longitude = properties.at("Longitude").get<double>();
          if (!coordinatesValid(latitude, longitude)) {
            return;
          }
          const auto description = properties.find("Description");
          m_latitude = latitude;
          m_longitude = longitude;
          m_name = description != properties.end() ? description->second.get<std::string>() : std::string{};
          if (m_name.empty()) {
            m_name = i18n::tr("location.locations.current");
          }
        } catch (const std::exception& e) {
          kLog.warn("malformed GeoClue location: {}", e.what());
          return;
        }
        m_resolved = true;
        m_resolvedAutoLocate = true;
        m_resolvedAddress = m_config.address;
        m_sourceLabel = i18n::tr("location.source.system");
        m_error.clear();
        // GeoClue pushes every later move itself.
        m_nextRefreshAt = Clock::now() + kIdleInterval;
        kLog.info("location resolved by GeoClue");
        saveCache();
        notifyChanged();
      });
}

void LocationService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::locationStatus, [this](const std::string&) -> std::string {
    const auto location = resolvedLocation();
    if (!location.has_value()) {
      return m_error.empty() ? "none\n" : "error: " + m_error + "\n";
    }
    return std::format(
        "{:.4f} {:.4f} {}: {}\n", location->latitude, location->longitude, location->sourceLabel, location->name
    );
  });
}

void LocationService::startGeolocate() {
  if (m_geoclueClient != nullptr && m_resolvedAutoLocate) {
    // GeoClue pushes every move to the running client.
    m_nextRefreshAt = Clock::now() + kIdleInterval;
    return;
  }
  // A client that never delivered a location (Start refused) is replaced.
  stopGeoclue();
  if (m_systemBus != nullptr && m_systemBus->nameHasOwner(std::string{kGeoclueName})) {
    startGeoclue();
    return;
  }
  m_error = i18n::tr("location.errors.geoclue-unavailable");
  kLog.warn("{}", m_error);
  scheduleRetryAfterFailure();
  notifyChanged();
}

void LocationService::startAddressGeocode() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "geocode.json";
  const std::string url =
      "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + uri::encodeComponent(m_config.address);
  const std::uint64_t serial = ++m_requestSerial;
  m_requestKind = RequestKind::GeocodeAddress;
  m_httpClient.download(url, path, [this, path, serial](bool success) {
    handleResponse(path, false, success, serial);
  });
}

void LocationService::handleResponse(
    const std::filesystem::path& path, bool autoLocated, bool success, std::uint64_t serial
) {
  if (serial != m_requestSerial) {
    return;
  }
  m_requestKind = RequestKind::None;

  if (!success) {
    m_error = autoLocated ? i18n::tr("location.errors.ip-geolocation-failed")
                          : i18n::tr("location.errors.address-lookup-failed");
    kLog.warn("{}", m_error);
    scheduleRetryAfterFailure();
    notifyChanged(); // serve last-known-good coordinates if we have them
    return;
  }

  try {
    std::ifstream file(path);
    // Open-Meteo omits `results` when nothing matches; at() then throws into the parse-error path.
    const auto json = nlohmann::json::parse(file).at("results").at(0);
    const double latitude = readNumber(json, "latitude");
    const double longitude = readNumber(json, "longitude");
    const std::string name = readString(json, "name");
    const std::string country = readString(json, "country");

    m_latitude = latitude;
    m_longitude = longitude;
    m_resolved = true;
    m_resolvedAutoLocate = autoLocated;
    m_resolvedAddress = m_config.address;
    m_sourceLabel = autoLocated ? i18n::tr("location.source.auto") : i18n::tr("location.source.address");
    m_name = compactLocationLabel(name, country);
    if (m_name.empty()) {
      m_name = autoLocated ? i18n::tr("location.locations.current") : m_config.address;
    }
    m_error.clear();
    m_nextRefreshAt = Clock::now() + kRefreshInterval;
    kLog.info("location resolved");
    saveCache();
    notifyChanged();
  } catch (const std::exception& e) {
    m_error =
        autoLocated ? i18n::tr("location.errors.parse-ip-geolocation") : i18n::tr("location.errors.parse-geocode");
    kLog.warn("{}: {}", m_error, e.what());
    scheduleRetryAfterFailure();
    notifyChanged();
  }
}

void LocationService::scheduleRetryAfterFailure() {
  m_refreshQueued = false;
  m_nextRefreshAt = Clock::now() + kRetryInterval;
}

void LocationService::clearResolved() {
  ++m_requestSerial;
  m_requestKind = RequestKind::None;
  m_resolved = false;
  m_resolvedAutoLocate = false;
  m_resolvedAddress.clear();
  m_name.clear();
  m_sourceLabel.clear();
  m_latitude = 0.0;
  m_longitude = 0.0;
  m_nextRefreshAt = Clock::time_point{};
}

bool LocationService::resolvedCoordinatesValid() const noexcept { return coordinatesValid(m_latitude, m_longitude); }

bool LocationService::manualCoordinatesValid() const noexcept {
  return m_config.latitude.has_value()
      && m_config.longitude.has_value()
      && coordinatesValid(*m_config.latitude, *m_config.longitude);
}

bool LocationService::coordinatesValid(double latitude, double longitude) noexcept {
  return std::isfinite(latitude)
      && std::isfinite(longitude)
      && latitude >= -90.0
      && latitude <= 90.0
      && longitude >= -180.0
      && longitude <= 180.0;
}

std::filesystem::path LocationService::transportCacheDir() {
  return stateCacheFilePath().parent_path() / "location";
}

std::filesystem::path LocationService::stateCacheFilePath() {
  if (const std::string cache = xdg::cacheHome(); !cache.empty()) {
    return std::filesystem::path(cache) / "andromeda" / "shell" / "location.json";
  }
  return std::filesystem::path(xdg::runtimeDir()) / "andromeda" / "shell" / "location.json";
}

std::string LocationService::compactLocationLabel(const std::string& name, const std::string& country) {
  if (!name.empty() && !country.empty()) {
    return std::format("{}, {}", name, country);
  }
  if (!name.empty()) {
    return name;
  }
  return country;
}

void LocationService::loadCache() {
  const auto path = stateCacheFilePath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }
  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    const bool cachedAutoLocate = readBool(json, "auto_locate");
    const std::string cachedAddress = readString(json, "address");
    if (cachedAutoLocate != m_config.autoLocate) {
      return;
    }
    if (!cachedAutoLocate && cachedAddress != m_config.address) {
      return;
    }

    m_latitude = readNumber(json, "latitude");
    m_longitude = readNumber(json, "longitude");
    if (!resolvedCoordinatesValid()) {
      m_latitude = 0.0;
      m_longitude = 0.0;
      return;
    }
    m_name = readString(json, "name");
    m_sourceLabel = readString(json, "source_label");
    m_resolvedAutoLocate = cachedAutoLocate;
    m_resolvedAddress = cachedAddress;
    m_resolved = true;
    m_nextRefreshAt = Clock::now() + kRefreshInterval;
    kLog.info("loaded cached location");
  } catch (const std::exception& e) {
    kLog.warn("failed to load location cache: {}", e.what());
  }
}

void LocationService::saveCache() const {
  if (!m_resolved) {
    return;
  }
  const auto path = stateCacheFilePath();

  const nlohmann::json json{
      {"auto_locate", m_resolvedAutoLocate},
      {"address", m_resolvedAddress},
      {"latitude", m_latitude},
      {"longitude", m_longitude},
      {"name", m_name},
      {"source_label", m_sourceLabel},
  };
  try {
    writeTextFileAtomicAsync(path, json.dump(2));
  } catch (const std::exception& e) {
    kLog.warn("failed to save location cache: {}", e.what());
  }
}
