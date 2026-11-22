#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace andromeda {

  // Submitted: the state is built and commits once the render thread ran its draw; the commit reports the result.
  enum class CommitResult { Applied, Pending, Submitted, Failed };
  enum class CommitOutcome { Ok, Busy, Failed };

  // The logical power state of an output; a failed configured-state commit restores the one it replaced.
  struct PowerState {
    bool desktopEnabled = false;
    bool dpmsOff = false;
    bool operator==(const PowerState&) const = default;
  };

  struct RequestedMode {
    int width = 0;
    int height = 0;
    int refreshMHz = 0;
    bool operator==(const RequestedMode&) const = default;
  };

  // `deferred`: the run waited for a frame event, so its requester already acted on Pending.
  struct ConfiguredRun {
    bool stageGeometry = false;
    PowerState restore{};
    bool deferred = false;
    bool operator==(const ConfiguredRun&) const = default;
  };

  // `restore` is set for a failure. A synchronous one restores the state before the request. A deferred one keeps the
  // requested desktop state (the layout already follows it) but restores the panel power, which the connector kept.
  struct ConfiguredFinish {
    CommitResult result = CommitResult::Applied;
    std::optional<PowerState> restore{};
  };

  // What a queued run waits for: Flip, the frame event (a page flip is pending); Render, the render-thread kick (a
  // frame or configured state is in flight, or the render thread holds the GL context).
  enum class ConfiguredWait : uint8_t { Flip, Render };

  // When an output's configured-state commit runs. A commit waits for the frame event while a page flip is pending
  // (a nonblocking commit would fail) and after EBUSY; a mode waits while the frame handler or a configured submit
  // commits (InFrame covers both).
  class ConfiguredCommit {
  public:
    struct Idle {
      bool operator==(const Idle&) const = default;
    };
    struct Queued {
      ConfiguredRun run{};
      ConfiguredWait wait = ConfiguredWait::Flip;
      bool operator==(const Queued&) const = default;
    };
    struct InFrame {
      std::optional<ConfiguredRun> queued{};
      std::optional<RequestedMode> mode{};
      bool operator==(const InFrame&) const = default;
    };
    using State = std::variant<Idle, Queued, InFrame>;

    [[nodiscard]] const State& state() const noexcept { return m_state; }
    [[nodiscard]] bool inFrame() const noexcept { return std::holds_alternative<InFrame>(m_state); }
    [[nodiscard]] bool waitsForRender() const noexcept {
      const auto* waiting = std::get_if<Queued>(&m_state);
      return waiting != nullptr && waiting->wait == ConfiguredWait::Render;
    }
    // A run waits: observers of its result (IPC replies, settle) wait with it.
    [[nodiscard]] bool pending() const { return queued().has_value(); }

    // Returns the run to commit now, or nothing when it waits (the caller reports Pending). Requests that wait merge:
    // geometry is staged when any asked for it, and a failure restores the oldest state.
    [[nodiscard]] std::optional<ConfiguredRun>
    request(bool stageGeometry, PowerState restore, std::optional<ConfiguredWait> wait) {
      ConfiguredRun run{.stageGeometry = stageGeometry, .restore = restore};
      if (const std::optional<ConfiguredRun> waiting = queued()) {
        run = {.stageGeometry = stageGeometry || waiting->stageGeometry, .restore = waiting->restore};
      }
      if (wait.has_value()) {
        setQueued(run, *wait);
        return std::nullopt;
      }
      setQueued(std::nullopt);
      return run;
    }

    // Reports the outcome of a run that request() or takeDue() returned. EBUSY is transient: it queues the run again.
    [[nodiscard]] ConfiguredFinish finish(const ConfiguredRun& run, CommitOutcome outcome, PowerState requested) {
      switch (outcome) {
      case CommitOutcome::Ok:
        return {.result = CommitResult::Applied};
      case CommitOutcome::Failed:
        return {.result = CommitResult::Failed,
                .restore = run.deferred
                    ? PowerState{.desktopEnabled = requested.desktopEnabled, .dpmsOff = run.restore.dpmsOff}
                    : run.restore};
      case CommitOutcome::Busy:
        break;
      }
      const std::optional<ConfiguredRun> waiting = queued();
      setQueued(ConfiguredRun{.stageGeometry = run.stageGeometry || (waiting.has_value() && waiting->stageGeometry),
                              .restore = run.restore});
      return {.result = CommitResult::Pending};
    }

    // The run that waited for the render thread, removed from the queue; nothing when it waits for another reason.
    [[nodiscard]] std::optional<ConfiguredRun> takeRenderReady() {
      if (!waitsForRender()) {
        return std::nullopt;
      }
      ConfiguredRun run = std::get<Queued>(m_state).run;
      run.deferred = true;
      m_state = Idle{};
      return run;
    }

    // Enters the frame handler; returns false (and changes nothing) when already in it. A queued run is then due.
    [[nodiscard]] bool beginFrame() {
      if (inFrame()) {
        return false;
      }
      std::optional<ConfiguredRun> queued{};
      if (auto* q = std::get_if<Queued>(&m_state)) {
        queued = q->run;
      }
      m_state = InFrame{.queued = queued};
      return true;
    }

    // The queued run, removed from the queue; only in the frame handler.
    [[nodiscard]] std::optional<ConfiguredRun> takeDue() {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return std::nullopt;
      }
      std::optional<ConfiguredRun> due = std::exchange(frame->queued, std::nullopt);
      if (due.has_value()) {
        due->deferred = true;
      }
      return due;
    }

    // Returns true when the mode waits for endFrame(); false when the caller commits it now.
    [[nodiscard]] bool requestMode(RequestedMode mode) {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return false;
      }
      frame->mode = mode;
      return true;
    }

    // Leaves the frame handler; returns the mode that waited for it. Outside the frame handler it changes nothing.
    // `next` is what a run queued meanwhile waits for.
    [[nodiscard]] std::optional<RequestedMode> endFrame(ConfiguredWait next = ConfiguredWait::Flip) {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return std::nullopt;
      }
      const std::optional<RequestedMode> mode = frame->mode;
      if (frame->queued.has_value()) {
        m_state = Queued{.run = *frame->queued, .wait = next};
      } else {
        m_state = Idle{};
      }
      return mode;
    }

  private:
    [[nodiscard]] std::optional<ConfiguredRun> queued() const {
      if (const auto* frame = std::get_if<InFrame>(&m_state)) {
        return frame->queued;
      }
      if (const auto* waiting = std::get_if<Queued>(&m_state)) {
        return waiting->run;
      }
      return std::nullopt;
    }
    // In the frame handler every wait ends with the frame: endFrame queues the run for the next frame event.
    void setQueued(std::optional<ConfiguredRun> run, ConfiguredWait wait = ConfiguredWait::Flip) {
      if (auto* frame = std::get_if<InFrame>(&m_state)) {
        frame->queued = run;
      } else if (run.has_value()) {
        m_state = Queued{.run = *run, .wait = wait};
      } else {
        m_state = Idle{};
      }
    }

    State m_state = Idle{};
  };

} // namespace andromeda
