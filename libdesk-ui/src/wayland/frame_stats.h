#pragma once

#include "platform/frame_stats_sink.h"

#include <cstdint>
#include <string>
#include <vector>

struct wl_surface;
struct wp_presentation;
struct wp_presentation_feedback;

// DESK_FRAME_STATS=1: one line per animation run of a surface, from wp_presentation feedback:
// "anim <name>#<run> frames N p99_cpu_ms X missed M discarded D". A run starts at the first frame rendered while springs are
// active and ends when the first frame after them is presented. CPU = tick + prepare + render time of each frame.
// missed = vblanks skipped between presented frames, not counting a gap after a tick that had nothing to draw.
class FrameStats final : public FrameStatsSink {
public:
  [[nodiscard]] static bool enabled();

  FrameStats(wp_presentation* presentation, wl_surface* surface, std::string name);
  ~FrameStats() override;
  FrameStats(const FrameStats&) = delete;
  FrameStats& operator=(const FrameStats&) = delete;

  void addCpu(float ms) noexcept override { m_cpuMs += ms; }
  void beginFrame(bool animating) override;
  // The compositor discards feedback for a commit with no new buffer, which is not a miss.
  void dropFrame() override;
  void idleTick() override;
  void endFrame() override;
  // Before a commit with no new buffer. The compositor discards the pending frame's feedback, but its buffer stays
  // on screen, so that discard is not a miss.
  void stateCommit();

  void onPresented(wp_presentation_feedback* feedback, std::uint64_t presentedNs, std::uint32_t refreshNs);
  void onDiscarded(wp_presentation_feedback* feedback);

private:
  void finishIfDone();

  wp_presentation* m_presentation;
  wl_surface* m_surface;
  std::string m_name;
  std::vector<wp_presentation_feedback*> m_pending;
  // Pending frames that follow a tick with nothing to draw: the gap before them is idle, not missed.
  std::vector<wp_presentation_feedback*> m_afterIdle;
  std::vector<wp_presentation_feedback*> m_beforeStateCommit;
  bool m_idleTick = false;
  std::vector<float> m_cpu;
  float m_cpuMs = 0.0F;
  bool m_active = false;
  bool m_ending = false;
  bool m_frameInRun = false;
  std::uint64_t m_lastPresentedNs = 0;
  std::uint32_t m_missed = 0;
  std::uint32_t m_discarded = 0; // replaced by a newer commit before display; part of m_missed
  std::uint32_t m_presented = 0;
  std::uint32_t m_run = 0;
};
