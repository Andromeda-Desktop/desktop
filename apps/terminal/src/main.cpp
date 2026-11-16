#include "terminal_app.h"
#include "theme_cli.h"

int main(int argc, char* argv[]) {
  if (const auto exit = term::runThemeCli(argc, argv)) {
    return *exit;
  }
  term::TerminalApp app;
  return app.run(argc, argv);
}
