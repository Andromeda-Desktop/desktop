#pragma once

// Collects per-frame CPU time and presentation results for one surface (DESK_FRAME_STATS=1).
class FrameStatsSink {
public:
  virtual ~FrameStatsSink() = default;

  virtual void addCpu(float ms) noexcept = 0;
  // Before the commit of a frame.
  virtual void beginFrame(bool animating) = 0;
  // The frame drew nothing new.
  virtual void dropFrame() = 0;
  // After the frame was rendered (its CPU time is complete).
  virtual void endFrame() = 0;
};
