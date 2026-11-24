#include "printing/print_settings.h"

#include <algorithm>
#include <print>
#include <random>
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
    expect(ranges.empty() && error && error->reason == RangeError::Reason::PastEnd && error->page == 7,
           "a page past the end names the page");
  }
  {
    const auto [ranges, error] = parsePageRanges("6, 1-2, 2-3", 6);
    expect(!error && ranges.size() == 2 && ranges[0].first == 1 && ranges[0].last == 3 && ranges[1].first == 6 &&
               ranges[1].last == 6,
           "ranges out of order or overlapping come back ascending and merged, as CUPS needs");
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

  // Random event sequences: one reply per request, nothing after Ended, a sent job never answers "cancelled".
  std::mt19937 rng(7);
  for (int run = 0; run < 2000; ++run) {
    PrintStage stage = PrintStage::SheetOpen;
    int replies = 0;
    bool sending = false;
    for (int step = 0; step < 12; ++step) {
      const auto event = static_cast<PrintEvent>(rng() % 5);
      const auto t = printTransition(stage, event);
      if (!t)
        continue;
      expect(stage != PrintStage::Ended, "no transition leaves Ended");
      if (t->reply) {
        ++replies;
        expect(!(sending && *t->reply == 1), "a job being sent is never answered as cancelled");
      }
      sending = sending || t->next == PrintStage::Submitting;
      stage = t->next;
    }
    expect(replies <= 1, "at most one reply per request");
    expect(stage != PrintStage::Ended || replies == 1, "an ended request was answered exactly once");
  }
  return g_failures == 0 ? 0 : 1;
}
