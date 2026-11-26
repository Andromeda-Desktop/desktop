#pragma once

#include "core/latest_request.h"
#include "config/config_types.h"
#include "dbus/name_owner_watch.h"

#include <sdbus-c++/IProxy.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ConfigService;
class HttpClient;
class IpcService;
class SystemBus;

namespace sdbus {
  class IProxy;
}

// Coordinates selected from IP geolocation, a geocoded address, or manual configuration.
struct ResolvedLocation {
  double latitude = 0.0;
  double longitude = 0.0;
  std::string name;        // display label, e.g. "Berlin, Germany"
  std::string sourceLabel; // i18n label describing how the location was resolved
};

// Owns "where am I" for the whole shell. Selects coordinates from the system location service (GeoClue), IP
// geolocation when GeoClue is absent, a geocoded address, or manual latitude/longitude and publishes them to consumers.
// Fixed sunrise/sunset schedules remain in LocationConfig. Runs independently of
// whether weather is enabled.
class LocationService {
public:
  using ChangeCallback = std::function<void()>;

  LocationService(ConfigService& configService, HttpClient& httpClient);
  ~LocationService();

  // Before initialize(): where GeoClue is looked for.
  void setSystemBus(SystemBus* bus) { m_systemBus = bus; }
  void registerIpc(IpcService& ipc);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  // auto_locate is on, or an address is set: a network resolution is requested.
  [[nodiscard]] bool networkResolutionConfigured() const noexcept;
  // Network resolution is requested but coordinates are not yet available.
  [[nodiscard]] bool resolving() const noexcept;
  [[nodiscard]] bool hasResolvedLocation() const noexcept;
  [[nodiscard]] std::optional<ResolvedLocation> resolvedLocation() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
  enum class RequestKind : std::uint8_t {
    None = 0,
    Geolocate = 1,
    GeocodeAddress = 2,
  };

  void onConfigReload();
  void notifyChanged();
  void requestRefresh();
  void startGeolocate();
  void startGeoclue();
  void startGeoclueClient(const sdbus::ObjectPath& clientPath);
  void stopGeoclue();
  void onGeoclueLocation(const std::string& path);
  void startAddressGeocode();
  void handleResponse(const std::filesystem::path& path, bool autoLocated, bool success, LatestRequest::Ticket ticket);
  void clearResolved();
  void scheduleRetryAfterFailure();
  void loadCache();
  void saveCache() const;
  [[nodiscard]] bool resolvedCoordinatesValid() const noexcept;
  [[nodiscard]] bool manualCoordinatesValid() const noexcept;
  [[nodiscard]] static bool coordinatesValid(double latitude, double longitude) noexcept;

  [[nodiscard]] static std::filesystem::path transportCacheDir();
  [[nodiscard]] static std::filesystem::path stateCacheFilePath();
  [[nodiscard]] static std::string compactLocationLabel(const std::string& name, const std::string& country);

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  SystemBus* m_systemBus = nullptr;
  std::unique_ptr<sdbus::IProxy> m_geoclueManager; // reset cancels a pending GetClient
  sdbus::PendingAsyncCall m_geoclueGetClient;
  std::unique_ptr<sdbus::IProxy> m_geoclueClient;
  std::unique_ptr<sdbus::IProxy> m_geoclueLocation;
  NameOwnerWatch m_geoclueOwner;
  LocationConfig m_config;
  std::vector<ChangeCallback> m_callbacks;

  bool m_resolved = false;
  double m_latitude = 0.0;
  double m_longitude = 0.0;
  std::string m_name;
  std::string m_sourceLabel;
  std::string m_resolvedAddress;
  bool m_resolvedAutoLocate = false;
  std::string m_error;

  RequestKind m_requestKind = RequestKind::None;
  LatestRequest m_request;
  bool m_refreshQueued = false;
  std::chrono::system_clock::time_point m_nextRefreshAt;
};
