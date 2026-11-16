#include "util/safe_regex.h"

#include <glib.h>

namespace {
  // All matches against one pattern, over every name of a folder, together.
  constexpr std::chrono::milliseconds kBudget{250};
} // namespace

std::optional<SafeRegex> SafeRegex::compile(const std::string& pattern, bool caseless) {
  GError* error = nullptr;
  GRegex* regex = g_regex_new(pattern.c_str(), static_cast<GRegexCompileFlags>(caseless ? G_REGEX_CASELESS : 0),
                              static_cast<GRegexMatchFlags>(0), &error);
  if (regex == nullptr) {
    g_clear_error(&error);
    return std::nullopt;
  }
  SafeRegex out;
  out.m_regex = std::shared_ptr<GRegex>(regex, g_regex_unref);
  out.m_deadline = std::chrono::steady_clock::now() + kBudget;
  return out;
}

bool SafeRegex::overBudget() const {
  if (!*m_exhausted && std::chrono::steady_clock::now() > m_deadline) {
    *m_exhausted = true;
  }
  return *m_exhausted;
}

bool SafeRegex::search(const std::string& text) const {
  if (overBudget() || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return false;
  }
  GError* error = nullptr;
  const bool found = g_regex_match_full(m_regex.get(), text.c_str(), static_cast<gssize>(text.size()), 0,
                                        static_cast<GRegexMatchFlags>(0), nullptr, &error) != 0;
  if (error != nullptr) {
    *m_exhausted = true; // the match limit
    g_clear_error(&error);
    return false;
  }
  return found;
}

bool SafeRegex::forEachMatch(std::string_view text, const std::function<void(std::size_t, std::size_t)>& found) const {
  if (overBudget() || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return false;
  }
  GMatchInfo* info = nullptr;
  GError* error = nullptr;
  g_regex_match_full(m_regex.get(), text.data(), static_cast<gssize>(text.size()), 0, static_cast<GRegexMatchFlags>(0),
                     &info, &error);
  while (error == nullptr && g_match_info_matches(info) != 0 && !overBudget()) {
    gint start = 0;
    gint end = 0;
    if (g_match_info_fetch_pos(info, 0, &start, &end) != 0 && end > start) {
      found(static_cast<std::size_t>(start), static_cast<std::size_t>(end));
    }
    g_match_info_next(info, &error);
  }
  g_match_info_free(info);
  if (error != nullptr) {
    *m_exhausted = true; // the match limit
    g_clear_error(&error);
  }
  return !*m_exhausted;
}

std::string SafeRegex::replace(const std::string& text, const std::string& replacement) const {
  if (overBudget() || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return text;
  }
  struct Ctx {
    const std::string* replacement;
  } ctx{&replacement};
  const auto eval = [](const GMatchInfo* info, GString* out, gpointer data) -> gboolean {
    const std::string& r = *static_cast<Ctx*>(data)->replacement;
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (r[i] != '$' || i + 1 == r.size()) {
        g_string_append_c(out, r[i]);
        continue;
      }
      const char next = r[i + 1];
      if (next == '$') {
        g_string_append_c(out, '$');
        ++i;
      } else if (next == '&' || (next >= '0' && next <= '9')) {
        gint group = next == '&' ? 0 : next - '0';
        ++i;
        if (next != '&' && i + 1 < r.size() && r[i + 1] >= '0' && r[i + 1] <= '9' &&
            group * 10 + (r[i + 1] - '0') < g_match_info_get_match_count(info)) {
          group = group * 10 + (r[++i] - '0'); // $12 when there are 12 groups
        }
        if (gchar* part = g_match_info_fetch(info, group)) {
          g_string_append(out, part);
          g_free(part);
        }
      } else {
        g_string_append_c(out, '$');
      }
    }
    return FALSE;
  };
  GError* error = nullptr;
  gchar* result = g_regex_replace_eval(m_regex.get(), text.c_str(), static_cast<gssize>(text.size()), 0,
                                       static_cast<GRegexMatchFlags>(0), eval, &ctx, &error);
  if (result == nullptr) {
    *m_exhausted = true;
    g_clear_error(&error);
    return text;
  }
  std::string out(result);
  g_free(result);
  return out;
}
