#include "launcher/math_provider.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/shared_worker.h"
#include "i18n/i18n.h"
#include "launcher/calc_engine.h"
#include "net/http_client.h"
#include "clipboard/clipboard_service.h"
#include "protocols/xdg_dirs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("math");
  // libqalculate loads its XML definitions in ~8 ms; this keeps them off the shell's first frame.
  constexpr auto kCalculatorLoadDelay = std::chrono::seconds(2);

  // Cheap pre-filter: the provider participates in global search on every keystroke,
  // so reject anything without a digit to avoid evaluating plain search text
  // (e.g. "firefox"). Letters/spaces are kept so unit and currency conversions
  // like "10 cm to in" or "5 USD to EUR" still reach libqalculate.
  bool looksLikeMath(std::string_view text) {
    return std::ranges::any_of(text, [&](char c) { return c >= '0' && c <= '9'; });
  }

  std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
      ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
      --end;
    }
    return std::string(text.substr(begin, end - begin));
  }

  constexpr auto kExchangeRateMaxAge = std::chrono::hours(24);

  bool shouldRefreshExchangeRateSource(std::string_view url, const std::filesystem::path& file) {
    return !url.contains("nbrb.by") && !HttpClient::isFresh(file, kExchangeRateMaxAge);
  }

  // libqalculate's getLocalDataDir(), without loading the library.
  std::filesystem::path qalculateDataDir() {
    if (const char* dir = std::getenv("QALCULATE_USER_DIR"); dir != nullptr && *dir != '\0') {
      return dir;
    }
    return std::filesystem::path(xdg::dataHome()) / "qalculate";
  }

  using CreateEngine = CalcEngine* (*)();

  // The module stays loaded for the process lifetime: engines run its code until they are destroyed.
  CreateEngine loadEngineModule() {
    static const CreateEngine create = []() -> CreateEngine {
      std::error_code ec;
      const auto exeDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
      for (const auto& path :
           {exeDir / "noctalia-calc.so",
            std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / NOCTALIA_INSTALL_LIBDIR / "noctalia/noctalia-calc.so"}) {
        void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr) {
          continue;
        }
        void* symbol = dlsym(module, kCalcEngineCreateSymbol);
        if (symbol == nullptr) {
          kLog.warn("{} has no {}; it is from another noctalia version", path.string(), kCalcEngineCreateSymbol);
          continue;
        }
        CreateEngine function = nullptr;
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
      }
      const char* error = dlerror();
      kLog.warn("calculator module noctalia-calc.so not loaded: {}", error != nullptr ? error : "");
      return nullptr;
    }();
    return create;
  }

} // namespace

MathProvider::MathProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient,
                           DirectoryWatcher* dirs)
    : m_clipboard(clipboard), m_config(config), m_httpClient(httpClient), m_dirs(dirs) {}

MathProvider::~MathProvider() {
  if (m_definitionsWatch >= 0) {
    m_dirs->unwatch(m_definitionsWatch);
  }
}

std::string MathProvider::displayName() const { return i18n::tr("launcher.providers.calculator.title"); }

void MathProvider::initialize() {
  m_definitionsDir = qalculateDataDir() / "definitions";
  watchDefinitionsDir();
  m_loadTimer.start(kCalculatorLoadDelay, [this] { loadCalculator(); });
}

void MathProvider::loadCalculator() { rebuildCalculator(); }

void MathProvider::onLauncherOpened() {
  if (!m_calc) {
    if (!m_loading) {
      m_loadTimer.stop();
      loadCalculator();
    }
    return;
  }

  bool changed = std::exchange(m_definitionsChanged, false);
  // No watch but the directory exists: it was created since the last open.
  if (m_definitionsWatch < 0 && watchDefinitionsDir()) {
    changed = true;
  }
  if (!changed) {
    return;
  }

  kLog.debug("libqalculate user definitions changed, reloading");
  rebuildCalculator();
}

void MathProvider::rebuildCalculator() {
  if (m_loading) {
    m_reloadAgain = true;
    return;
  }
  // Calculator::reset() leaves lazily cached items (e.g. the radian unit) dangling, full recreation instead.
  // Destroy the old engine first: libqalculate keeps one global Calculator pointer, which the new one sets.
  m_calc.reset();
  m_loading = true;
  SharedWorker::instance().post<std::unique_ptr<CalcEngine>>(
      m_alive,
      [] {
        const CreateEngine create = loadEngineModule();
        return std::unique_ptr<CalcEngine>(create != nullptr ? create() : nullptr);
      },
      [this](std::unique_ptr<CalcEngine> engine) {
        m_loading = false;
        m_calc = std::move(engine);
        if (std::exchange(m_reloadAgain, false)) {
          rebuildCalculator();
        }
      }
  );
}

bool MathProvider::watchDefinitionsDir() {
  std::error_code ec;
  if (m_dirs == nullptr || !std::filesystem::is_directory(m_definitionsDir, ec)) {
    return false;
  }
  m_definitionsWatch = m_dirs->watch(m_definitionsDir, [this](DirectoryWatcher::Event event, const std::string&,
                                                              const std::string&) {
    m_definitionsChanged = true;
    if (event == DirectoryWatcher::Event::Gone) {
      m_dirs->unwatch(std::exchange(m_definitionsWatch, -1));
    }
  });
  return m_definitionsWatch >= 0;
}

void MathProvider::refreshExchangeRates() const {
  if (m_exchangeRatesChecked || m_httpClient == nullptr || m_config == nullptr || !m_calc) {
    return;
  }
  m_exchangeRatesChecked = true;
  if (m_config->config().shell.offlineMode
      || !m_config->config().shell.launcher.fetchExchangeRates
      || !m_calc->canFetchExchangeRates()) {
    return;
  }

  std::vector<std::pair<std::string, std::filesystem::path>> sources;
  for (auto& [url, file] : m_calc->exchangeRateSources()) {
    std::filesystem::path path(std::move(file));
    if (shouldRefreshExchangeRateSource(url, path)) {
      sources.emplace_back(std::move(url), std::move(path));
    }
  }
  if (sources.empty()) {
    return;
  }

  auto remaining = std::make_shared<int>(static_cast<int>(sources.size()));
  for (auto& [url, path] : sources) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    m_httpClient->download(url, path, [this, remaining](bool) {
      if (--(*remaining) == 0) {
        // All sources settled; reload whatever landed on disc.
        m_calc->loadExchangeRates();
      }
    });
  }
}

std::vector<LauncherResult> MathProvider::query(std::string_view text) const {
  if (!looksLikeMath(text)) {
    return {};
  }
  return evaluate(text);
}

std::vector<LauncherResult> MathProvider::queryPrefixed(std::string_view text) const { return evaluate(text); }

std::vector<LauncherResult> MathProvider::evaluate(std::string_view text) const {
  const std::string localized = trimmed(text);
  if (!m_calc || localized.empty()) {
    return {};
  }

  std::string input;
  const auto output = m_calc->evaluate(localized, input);
  // Reject errors and no-ops (e.g. the user just typed a bare number).
  if (!output.has_value() || output->empty() || *output == input || *output == localized) {
    return {};
  }

  refreshExchangeRates();

  LauncherResult r;
  r.id = "math";
  r.title = "= " + *output;
  r.subtitle = std::string(text);
  r.glyphName = "calculator";
  r.score = 10000;

  return {std::move(r)};
}

bool MathProvider::activate(const LauncherResult& result) {
  if (result.id != "math") {
    return false;
  }

  std::string value = result.title.substr(2);
  return m_clipboard != nullptr && m_clipboard->copyText(std::move(value));
}
