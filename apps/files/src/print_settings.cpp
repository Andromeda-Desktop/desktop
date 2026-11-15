#include "print_settings.h"

#include <charconv>
#include <string_view>

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

std::optional<int> number(std::string_view s) {
  s = trim(s);
  int value = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || end != s.data() + s.size())
    return std::nullopt;
  return value;
}

} // namespace

std::pair<std::vector<PageRange>, std::optional<RangeError>> parsePageRanges(const std::string& text, int pageCount) {
  std::vector<PageRange> ranges;
  const auto fail = [&](std::string message) {
    return std::pair{std::vector<PageRange>{}, std::optional<RangeError>{RangeError{std::move(message)}}};
  };
  std::string_view rest = text;
  while (true) {
    const auto comma = rest.find(',');
    const std::string_view part = trim(rest.substr(0, comma));
    if (!part.empty()) {
      const auto dash = part.find('-');
      const std::optional<int> first = number(part.substr(0, dash));
      const std::optional<int> last = dash == std::string_view::npos ? first : number(part.substr(dash + 1));
      if (!first || !last)
        return fail("Use page numbers and ranges, for example 1-2, 4.");
      if (*first < 1 || *last < *first)
        return fail("“" + std::string(part) + "” is not a valid range.");
      if (*last > pageCount)
        return fail("Page " + std::to_string(*last) + " does not exist. This document has " + std::to_string(pageCount) +
                    (pageCount == 1 ? " page." : " pages."));
      ranges.push_back({*first, *last});
    }
    if (comma == std::string_view::npos)
      break;
    rest.remove_prefix(comma + 1);
  }
  if (ranges.empty())
    return fail("Type the pages to print, for example 1-2, 4.");
  return {std::move(ranges), std::nullopt};
}

std::vector<std::pair<std::string, std::string>> cupsJobOptions(const PrintOptions& o) {
  std::vector<std::pair<std::string, std::string>> out;
  out.emplace_back("copies", std::to_string(o.copies));
  if (o.copies > 1)
    out.emplace_back("multiple-document-handling",
                     o.collate ? "separate-documents-collated-copies" : "separate-documents-uncollated-copies");
  if (o.ranges) {
    std::string value;
    for (const PageRange& r : *o.ranges) {
      if (!value.empty())
        value += ',';
      value += std::to_string(r.first) + '-' + std::to_string(r.last);
    }
    out.emplace_back("page-ranges", value);
  }
  switch (o.duplex) {
  case Duplex::Off:
    out.emplace_back("sides", "one-sided");
    break;
  case Duplex::LongEdge:
    out.emplace_back("sides", "two-sided-long-edge");
    break;
  case Duplex::ShortEdge:
    out.emplace_back("sides", "two-sided-short-edge");
    break;
  }
  out.emplace_back("print-color-mode", o.colour ? "color" : "monochrome");
  if (!o.media.empty())
    out.emplace_back("media", o.media);
  if (o.numberUp > 1)
    out.emplace_back("number-up", std::to_string(o.numberUp));
  if (o.orientation == Orientation::Portrait)
    out.emplace_back("orientation-requested", "3");
  else if (o.orientation == Orientation::Landscape)
    out.emplace_back("orientation-requested", "4");
  switch (o.scaling) {
  case Scaling::Fit:
    out.emplace_back("print-scaling", "fit");
    break;
  case Scaling::Actual:
    out.emplace_back("print-scaling", "none");
    break;
  case Scaling::Custom:
    out.emplace_back("print-scaling", "none");
    out.emplace_back("scaling", std::to_string(o.scalePercent));
    break;
  }
  for (const auto& kv : o.printerOptions)
    out.push_back(kv);
  return out;
}

std::optional<std::pair<double, double>> mediaSizeMm(const std::string& pwgName) {
  const auto underscore = pwgName.rfind('_');
  if (underscore == std::string::npos)
    return std::nullopt;
  std::string_view dims = std::string_view(pwgName).substr(underscore + 1);
  double factor = 0;
  if (dims.ends_with("mm"))
    factor = 1.0;
  else if (dims.ends_with("in"))
    factor = 25.4;
  else
    return std::nullopt;
  dims.remove_suffix(2);
  const auto x = dims.find('x');
  if (x == std::string_view::npos)
    return std::nullopt;
  double w = 0;
  double h = 0;
  const auto parse = [](std::string_view s, double& out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && end == s.data() + s.size() && out > 0;
  };
  if (!parse(dims.substr(0, x), w) || !parse(dims.substr(x + 1), h))
    return std::nullopt;
  return std::pair{w * factor, h * factor};
}

std::optional<PrintTransition> printTransition(PrintStage stage, PrintEvent event) {
  switch (stage) {
  case PrintStage::SheetOpen:
    switch (event) {
    case PrintEvent::UserPrint:
      return PrintTransition{PrintStage::Submitting, std::nullopt};
    case PrintEvent::UserCancel:
      return PrintTransition{PrintStage::Ended, 1U};
    case PrintEvent::CallerClose:
      return PrintTransition{PrintStage::Ended, 2U};
    default:
      return std::nullopt;
    }
  case PrintStage::Submitting:
    switch (event) {
    case PrintEvent::JobSent:
      return PrintTransition{PrintStage::Ended, 0U};
    case PrintEvent::JobFailed:
      return PrintTransition{PrintStage::Ended, 2U};
    case PrintEvent::CallerClose:
      return PrintTransition{PrintStage::CallerGone, 2U};
    default:
      return std::nullopt;
    }
  case PrintStage::CallerGone:
    if (event == PrintEvent::JobSent || event == PrintEvent::JobFailed)
      return PrintTransition{PrintStage::Ended, std::nullopt};
    return std::nullopt;
  case PrintStage::Ended:
    return std::nullopt;
  }
  return std::nullopt;
}

std::string mediaLabel(const std::string& pwgName) {
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"iso_a3", "A3"},         {"iso_a4", "A4"},       {"iso_a5", "A5"},         {"iso_a6", "A6"},
      {"iso_b5", "B5"},         {"jis_b5", "B5 (JIS)"}, {"na_letter", "Letter"},  {"na_legal", "Legal"},
      {"na_executive", "Executive"}, {"na_ledger", "Tabloid"}, {"iso_dl", "Envelope DL"}, {"na_number-10", "Envelope #10"},
      {"na_index-4x6", "4 × 6 in"}, {"na_5x7", "5 × 7 in"}, {"oe_photo-l", "Photo L"},
  };
  const auto underscore = pwgName.rfind('_');
  const std::string_view prefix = std::string_view(pwgName).substr(0, underscore);
  for (const auto& [key, label] : kNames)
    if (prefix == key)
      return std::string(label);
  if (const auto size = mediaSizeMm(pwgName))
    return std::to_string(static_cast<int>(size->first + 0.5)) + " × " + std::to_string(static_cast<int>(size->second + 0.5)) +
           " mm";
  return pwgName;
}
