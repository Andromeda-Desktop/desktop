#include "print_settings.h"

#include <algorithm>
#include <print>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "print_settings_test: {}", what);
      ++g_failures;
    }
  }

  std::string option(const PrintOptions& o, const std::string& key) {
    const auto opts = cupsJobOptions(o);
    const auto it = std::find_if(opts.begin(), opts.end(), [&](const auto& kv) { return kv.first == key; });
    return it == opts.end() ? "" : it->second;
  }
} // namespace

int main() {
  {
    const auto [ranges, error] = parsePageRanges(" 1-2, 4 ,", 4);
    expect(!error && ranges.size() == 2 && ranges[0].first == 1 && ranges[0].last == 2 && ranges[1].first == 4 &&
               ranges[1].last == 4,
           "\"1-2, 4\" parses to two ranges");
  }
  {
    const auto [ranges, error] = parsePageRanges("1-3, 7", 4);
    expect(ranges.empty() && error && error->message.contains("Page 7") && error->message.contains("4 pages"),
           "a page past the end names the page and the page count");
  }
  expect(parsePageRanges("3-1", 4).second.has_value(), "a reversed range is rejected");
  expect(parsePageRanges("0", 4).second.has_value(), "page 0 is rejected");
  expect(parsePageRanges("a-b", 4).second.has_value(), "words are rejected");
  expect(parsePageRanges(" ", 4).second.has_value(), "an empty range is rejected");

  PrintOptions o;
  o.copies = 2;
  o.collate = false;
  o.ranges = std::vector<PageRange>{{1, 2}, {4, 4}};
  o.duplex = Duplex::ShortEdge;
  o.colour = false;
  o.numberUp = 2;
  o.orientation = Orientation::Landscape;
  expect(option(o, "copies") == "2", "copies");
  expect(option(o, "multiple-document-handling") == "separate-documents-uncollated-copies", "collate off");
  expect(option(o, "page-ranges") == "1-2,4-4", "page-ranges");
  expect(option(o, "sides") == "two-sided-short-edge", "short edge");
  expect(option(o, "print-color-mode") == "monochrome", "black and white");
  expect(option(o, "number-up") == "2", "number-up");
  expect(option(o, "orientation-requested") == "4", "landscape");
  return g_failures == 0 ? 0 : 1;
}
