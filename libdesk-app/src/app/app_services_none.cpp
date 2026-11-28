#include "app/app_services.h"
#include "ui/palette.h"

void showInFinder(const std::vector<std::string>& paths); // show_in_finder_macos.mm

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
    void setColorScheme(ColorScheme scheme) override {
      const bool light = scheme == ColorScheme::Light;
      setResolvedThemeLight(light);
      setPalette(tokenPalette(light));
    }
    [[nodiscard]] SessionBus* bus() override { return nullptr; }
    void showInFolder(const std::vector<std::string>& paths) override { showInFinder(paths); }
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

std::vector<std::string> serveAppZygote(
    const std::string& /*name*/, const std::function<void()>& /*warmup*/,
    const std::function<bool(const std::vector<std::string>& argv)>& /*accept*/
) {
  return {};
}
