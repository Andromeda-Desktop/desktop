#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

// Lock-before-sleep for the shell and the lock app. The compositor holds each sleep until its session lock is on
// screen or the shell sends sleep_ready (a sleep without a lock). The caller owns the lock; this class owns the phase.
class LockBeforeSleep {
public:
  enum class Phase : std::uint8_t { Awake, AwakeSkipNextLock, LockingForSleep, ReadyForSleep };

  struct Hooks {
    std::function<bool()> enabled;
    std::function<bool()> lockReadyForSleep;
    // Locked, or a lock on its way (a relock queued behind an unlock counts).
    std::function<bool()> lockHeldOrQueued;
    // The lock runs its exit transition or a cancelled request.
    std::function<bool()> lockTearingDown;
    // False when the lock refuses to start.
    std::function<bool()> lock;
    std::function<void()> prepareLockForSleep;
    // dsk_shell_v1.sleep_ready: the user chose a sleep without a lock. Every other sleep waits for the compositor to
    // present the session lock, or to lock behind a blank when the lock does not come in time.
    std::function<void()> sleepReady;
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
  };

  // A detached suspend reports only its launch; a skip whose sleep never comes must not skip a later lid sleep.
  static constexpr std::chrono::seconds kSkipLifetime{10};

  explicit LockBeforeSleep(Hooks hooks) : m_hooks(std::move(hooks)) {}

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }

  // A suspend the session starts itself (plain Suspend, or already locked) skips the lock.
  // A skip asked for during an unlock belongs to the lock that goes away: the next sleep still locks.
  void setSkipNextLock(bool skip) {
    if (skip && m_hooks.lockTearingDown()) {
      return;
    }
    if (m_phase == Phase::Awake || m_phase == Phase::AwakeSkipNextLock) {
      m_phase = skip ? Phase::AwakeSkipNextLock : Phase::Awake;
      m_skipUntil = m_hooks.now() + kSkipLifetime;
    }
  }

  void onPrepareForSleep(bool sleeping) {
    if (!sleeping) {
      m_phase = Phase::Awake;
      return;
    }
    const bool skip = m_phase == Phase::AwakeSkipNextLock && m_hooks.now() < m_skipUntil;
    if (skip || !m_hooks.enabled()) {
      ready();
      m_hooks.sleepReady();
      return;
    }
    if (m_hooks.lockReadyForSleep()) {
      ready();
      return;
    }
    // Set first: lock() and prepareLockForSleep() may settle the lock before they return.
    m_phase = Phase::LockingForSleep;
    if (!m_hooks.lockHeldOrQueued() && !m_hooks.lock()) {
      ready();
      return;
    }
    m_hooks.prepareLockForSleep();
    // A deferred lock (no outputs yet) never reaches SessionLocked: the compositor locks behind its blank.
    if (m_phase == Phase::LockingForSleep && !m_hooks.lockHeldOrQueued()) {
      ready();
    }
  }

  // The lock frames are on screen, or their deadline passed: the compositor acks once the lock is presented.
  void onLockSettled() {
    if (m_phase == Phase::LockingForSleep) {
      ready();
    }
  }

  // The lock ended (unlocked or aborted). A relock queued behind it keeps the phase until it settles.
  void onLockEnded() {
    if (m_phase == Phase::LockingForSleep && !m_hooks.lockHeldOrQueued()) {
      ready();
    }
  }

private:
  void ready() { m_phase = Phase::ReadyForSleep; }

  Hooks m_hooks;
  Phase m_phase = Phase::Awake;
  std::chrono::steady_clock::time_point m_skipUntil;
};
