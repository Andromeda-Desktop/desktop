#include "net/uri.h"

#include <glib.h>

#include <optional>
#include <print>
#include <random>
#include <string>
#include <vector>

namespace {

  bool check(bool condition, const std::string& context) {
    if (!condition) {
      std::println(stderr, "uri_codec_test: {}", context);
    }
    return condition;
  }

  std::optional<std::string> glibPath(const std::string& text) {
    gchar* path = g_filename_from_uri(text.c_str(), nullptr, nullptr);
    if (path == nullptr) {
      return std::nullopt;
    }
    std::string out(path);
    g_free(path);
    return out;
  }

  std::string glibUri(const std::string& path) {
    gchar* text = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = text != nullptr ? text : "";
    g_free(text);
    return out;
  }

} // namespace

int main() {
  std::mt19937 rng(0xC17);
  auto pick = [&](std::string_view from) { return from[std::uniform_int_distribution<std::size_t>(0, from.size() - 1)(rng)]; };
  auto len = [&](int most) { return std::uniform_int_distribution<int>(0, most)(rng); };
  auto anyByte = [&] { return static_cast<char>(std::uniform_int_distribution<int>(0, 255)(rng)); };
  // Path bytes: every class the encoder treats differently, plus UTF-8 lead and continuation bytes.
  constexpr std::string_view kPathBytes = "aZ09-_.~!$&'()*+,/:=@ %#?\\\"<>[]{}|^`;\t\xC3\xA9\xFF";
  constexpr std::string_view kUriBytes = "aZ09-_./%%%2F0fgG+:@ ?#";
  constexpr std::string_view kListBytes = "aZ09-_./%%%2F0fgG+:@?#";

  bool ok = true;
  for (int round = 0; round < 3000 && ok; ++round) {
    std::string bytes;
    for (int i = len(24); i > 0; --i) {
      bytes.push_back(anyByte());
    }
    const std::string encoded = uri::encodeComponent(bytes);
    ok = check(uri::decodeComponent(encoded) == bytes, "decode(encode(x)) != x") && ok;
    ok = check(uri::decodeComponentStrict(encoded) == bytes, "strict decode(encode(x)) != x") && ok;
    ok = check(encoded.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~%")
                   == std::string::npos,
               "encode left a reserved byte")
        && ok;

    std::string path = "/";
    for (int i = len(20); i > 0; --i) {
      path.push_back(pick(kPathBytes));
    }
    const std::string fileUri = uri::fileUri(path);
    ok = check(fileUri == glibUri(path), "fileUri differs from g_filename_to_uri for " + path) && ok;
    ok = check(uri::pathFromFileUri(fileUri) == path, "pathFromFileUri(fileUri(p)) != p for " + path) && ok;

    // Hand-made local URIs without query or fragment: the parser accepts and rejects what GLib does.
    std::string local = "file:///";
    for (int i = len(16); i > 0; --i) {
      local.push_back(pick(kUriBytes.substr(0, kUriBytes.size() - 2)));
    }
    ok = check(uri::pathFromFileUri(local) == glibPath(local), "pathFromFileUri differs from GLib for " + local) && ok;

    std::vector<std::string> uris;
    for (int i = len(5); i > 0; --i) {
      std::string item = "file:///";
      for (int j = len(10); j > 0; --j) {
        item.push_back(pick(kListBytes));
      }
      uris.push_back(item);
    }
    ok = check(uri::parseUriList(uri::joinUriList(uris)) == uris, "parseUriList(joinUriList(x)) != x") && ok;
  }

  ok = check(uri::pathFromFileUri("file://localhost/tmp/a%20b") == "/tmp/a b", "localhost host") && ok;
  ok = check(!uri::pathFromFileUri("file://remote/tmp/a"), "remote host accepted") && ok;
  ok = check(uri::pathFromFileUri("file:///tmp/a%23b?q#f") == "/tmp/a#b", "query or fragment kept") && ok;
  ok = check(uri::parseUriList("# comment\r\nfile:///a \r\n\nfile:///b") == std::vector<std::string>{"file:///a", "file:///b"},
             "uri-list comments, blanks or trailing space")
      && ok;
  ok = check(uri::decodeComponent("a+b%2", uri::Plus::Space) == "a b%2", "form decode") && ok;
  return ok ? 0 : 1;
}
