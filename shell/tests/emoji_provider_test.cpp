#include "launcher/emoji_provider.h"

#include <cstdlib>
#include <print>
#include <string_view>

namespace {

  int failures = 0;

  void expectFirst(const EmojiProvider& provider, std::string_view query, std::string_view badge) {
    const auto results = provider.query(query);
    if (results.empty() || results.front().badge != badge) {
      std::println(stderr, "emoji_provider_test: '{}' gave '{}', want '{}'", query,
                   results.empty() ? "nothing" : results.front().badge, badge);
      ++failures;
    }
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return 2;
  }
  setenv("ANDROMEDA_ASSETS_DIR", argv[1], 1);
  EmojiProvider provider(nullptr, nullptr);
  provider.initialize();

  expectFirst(provider, "em dash", "—");
  expectFirst(provider, "U+2014", "—");
  expectFirst(provider, "2014", "—");
  expectFirst(provider, "greek small letter omega", "ω");
  expectFirst(provider, "grinning face", "\U0001F600");
  return failures == 0 ? 0 : 1;
}
