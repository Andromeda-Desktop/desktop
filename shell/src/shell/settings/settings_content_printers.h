#pragma once

#include <functional>
#include <string>

class Flex;
class PrintersModel;

namespace settings {

  struct SettingsPrintersContext {
    float scale = 1.0F;
    PrintersModel* model = nullptr;
    // "printer:<label>" with the CUPS queue name after a tab while a printer's detail page is open, else empty.
    std::string detail;
    std::function<void(std::string)> openDetail;
    std::function<void()> back;
    std::function<void()> addByAddress;
  };

  // Printers page (direction A, .agent/DESIGN.md "Settings › Printers"): My printers with their jobs, Nearby
  // printers; or one printer's details.
  void addSettingsPrinters(Flex& content, const SettingsPrintersContext& ctx);

} // namespace settings
