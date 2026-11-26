#pragma once

#include "config/value_parse.h"
#include "scene/effect_audio.h"

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace andromeda {

  // Sends a deferred handler's reply; a reply after the connection closed or timed out is dropped.
  using IpcReply = std::function<void(nlohmann::json)>;

  class Server;

  struct IpcCommands {
    static nlohmann::json keyboardLayouts(Server& server, std::string_view arg);
    static nlohmann::json inputCapturePermissions(Server& server, std::string_view arg);
    static void inputCaptureRevoke(Server& server, std::string_view arg, IpcReply reply);
    static nlohmann::json trustedBinaries(Server& server, std::string_view arg);
    static void trustedBinaryAdd(Server& server, std::string_view arg, IpcReply reply);
    static void trustedBinaryRemove(Server& server, std::string_view arg, IpcReply reply);
    static nlohmann::json layerShellPermissions(Server& server, std::string_view arg);
    static void layerShellRevoke(Server& server, std::string_view arg, IpcReply reply);
    static nlohmann::json globalShortcuts(Server& server, std::string_view arg);
    static void globalShortcutsRevoke(Server& server, std::string_view arg, IpcReply reply);
    static void globalShortcutsAssign(Server& server, std::string_view arg, IpcReply reply);
    [[nodiscard]] static std::optional<EffectAudio> parseAudioSpectrum(const nlohmann::json& request);
    static nlohmann::json effects(Server& server, std::string_view arg);
    static nlohmann::json windows(Server& server, std::string_view arg);
    static nlohmann::json focusedWindow(Server& server, std::string_view arg);
    static nlohmann::json appMenu(Server& server, std::string_view arg);
    static nlohmann::json appMenuActivate(Server& server, std::string_view arg);
    static nlohmann::json workspaces(Server& server, std::string_view arg);
    static nlohmann::json submap(Server& server, std::string_view arg);
    static nlohmann::json layers(Server& server, std::string_view arg);
    static nlohmann::json outputs(Server& server, std::string_view arg);
    static nlohmann::json color(Server& server, std::string_view arg);
    static nlohmann::json zoom(Server& server, std::string_view arg);
    static nlohmann::json tearing(Server& server, std::string_view arg);
    static nlohmann::json msg(Server& server, std::string_view arg);
    static nlohmann::json outputCreate(Server& server, std::string_view arg);
    static nlohmann::json outputDestroy(Server& server, std::string_view arg);
    static nlohmann::json configReplace(Server& server, std::string_view arg);
    static nlohmann::json configSet(Server& server, std::string_view arg);
    static nlohmann::json configGet(Server& server, std::string_view arg);
    static nlohmann::json configUnset(Server& server, std::string_view arg);
    // The reply to a settle request. The IPC server holds the request until the compositor is settled.
    static nlohmann::json settle(Server& server, std::string_view arg);
    static nlohmann::json clockFreeze(Server& server, std::string_view arg);
    static nlohmann::json stallSubmit(Server& server, std::string_view arg);
    static nlohmann::json sleepSignal(Server& server, std::string_view arg);
    static nlohmann::json outputFailNextCommit(Server& server, std::string_view arg);
    static nlohmann::json stallFrame(Server& server, std::string_view arg);
    // The reply to clock-advance. The IPC server advances the clock and holds the reply until every output has drawn.
    static nlohmann::json clockAdvance(Server& server, std::string_view arg);
    static nlohmann::json clockResume(Server& server, std::string_view arg);
    static nlohmann::json logLevel(Server& server, std::string_view arg);
#ifdef ANDROMEDA_TEST_IPC
    static nlohmann::json rendererRecover(Server& server, std::string_view arg);
    static nlohmann::json outputCommits(Server& server, std::string_view arg);
    static nlohmann::json outputTestModes(Server& server, std::string_view arg);
    static nlohmann::json swipeForTest(Server& server, std::string_view arg);
    static nlohmann::json pinchForTest(Server& server, std::string_view arg);
    static nlohmann::json animationCaptureFail(Server& server, std::string_view arg);
    static nlohmann::json lockKeysForTest(Server& server, std::string_view arg);
    static nlohmann::json blurCompositeSplit(Server& server, std::string_view arg);
    static nlohmann::json planeCursor(Server& server, std::string_view arg);
    static nlohmann::json effectFrames(Server& server, std::string_view arg);
    static nlohmann::json blurCachePixels(Server& server, std::string_view arg);
    static nlohmann::json cursorState(Server& server, std::string_view arg);
#endif
  };

  // The `andromeda --help` section that lists a command.
  enum class IpcCommandGroup {
    Control,
    // Spelled `andromeda config <verb>` on the command line; the request names it `config-<verb>`.
    Config,
    Inspect,
    Harness,
  };

  // The CLI wait for a command that can change an output: its reply waits for the configured commit, which can wait
  // for a stalled GPU submit (2.5 s seen on xe).
  inline constexpr int kConfiguredReplyTimeoutSec = 10;
  // The compositor answers a configured wait with an error before the CLI gives up.
  inline constexpr int kConfiguredWaitTimeoutMs = (kConfiguredReplyTimeoutSec - 2) * 1000;
  // The CLI wait for a command that saves a state file: an fsync under heavy disk load can take seconds.
  inline constexpr int kSaveReplyTimeoutSec = 10;
  inline constexpr int kSaveWaitTimeoutMs = (kSaveReplyTimeoutSec - 2) * 1000;

  using IpcHandler = nlohmann::json (*)(Server& server, std::string_view arg);
  // For a command that waits on a job (a state save): it answers through `reply`, now or later.
  using IpcDeferredHandler = void (*)(Server& server, std::string_view arg, IpcReply reply);

  struct IpcCommandSpec {
    std::string_view name;
    std::string_view argSpec;
    std::string_view description;
    IpcCommandGroup group;
    bool takesArg;
    std::variant<IpcHandler, IpcDeferredHandler> handle;
    void (*printHuman)(const nlohmann::json& ok);
    // How long the CLI waits for the reply.
    int replyTimeoutSec = 2;
  };

  std::span<const IpcCommandSpec> ipcCommands();
  const IpcCommandSpec* findIpcCommand(std::string_view name);
  // How the command line spells a command: `config set` for `config-set`, the request name otherwise.
  std::string cliName(const IpcCommandSpec& spec);

  // Splits an output-create argument into the output name and its optional mode, in the config `mode` format
  // ("WIDTHxHEIGHT[@HZ]"). Returns false with `error` set on anything else.
  bool
  parseOutputCreateArg(std::string_view arg, std::string& name, std::optional<OutputMode>& mode, std::string& error);

} // namespace andromeda
