#include "app/app_services.h"

namespace {
  // No session services: every launch runs alone, with the default appearance.
  class NoServices final : public AppServices {
  public:
    bool claim(const std::vector<std::string>& /*uris*/, const std::string& /*action*/) override { return true; }
    void attach(AppLoop& /*loop*/) override {}
    void detach(AppLoop& /*loop*/) override {}
    void processPendingEvents() override {}
    [[nodiscard]] bool hasAppearance() const override { return false; }
    [[nodiscard]] float uiScale() const override { return 1.0F; }
    [[nodiscard]] float textScale() const override { return 1.0F; }
    [[nodiscard]] bool holdMotion() const override { return false; }
    [[nodiscard]] SessionBus* bus() override { return nullptr; }
  };
} // namespace

std::unique_ptr<AppServices> createAppServices(
    const std::string& /*appId*/, const std::string& /*appName*/, bool /*singleInstance*/,
    AppServices::Callbacks /*callbacks*/
) {
  return std::make_unique<NoServices>();
}

bool forwardLaunch(const std::string& /*appId*/, const std::vector<std::string>& /*uris*/, const std::string& /*action*/) {
  return false;
}

std::vector<std::string> serveAppZygote(const std::string& /*appId*/, const std::function<void()>& /*warmup*/) {
  return {};
}
