#include "system/crash_report.h"

#include "core/process/process.h"
#include "protocols/xdg_dirs.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace crash_report {

  std::filesystem::path directory() {
    return std::filesystem::path(xdg::stateHome()) / "andromeda/crash-reports";
  }

  void save(int pid) {
    const std::string match = pid > 0 ? std::to_string(pid) : "-1 andromeda";
    (void)process::runAsync(process::shellArgv(
        "mkdir -p \"$1\"; f=\"$1/$(date +%Y%m%d-%H%M%S).txt\"; "
        "{ echo \"Andromeda crash report $(date -Is)\"; coredumpctl info --no-pager $2 2>&1; echo; "
        "journalctl --user -u andromeda.service -n 300 --no-pager 2>&1; } > \"$f\"; xdg-open \"$f\"",
        {directory().string(), match}
    ));
  }

  void openDirectory() {
    std::error_code ec;
    std::filesystem::create_directories(directory(), ec);
    (void)process::runAsync(std::vector<std::string>{"xdg-open", directory().string()});
  }

} // namespace crash_report
