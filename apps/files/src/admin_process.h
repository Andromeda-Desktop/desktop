#pragma once

#include "admin_session.h"

#include <functional>
#include <string>

typedef struct _GSubprocess GSubprocess;
typedef struct _GDataInputStream GDataInputStream;
typedef struct _GOutputStream GOutputStream;
typedef struct _GCancellable GCancellable;

// Runs `pkexec andromeda-fileops-admin serve` for one administrator tab and connects it to an AdminSession.
// Destroying it closes the helper's stdin, so the helper ends (also when it starts only after the tab closed).
class AdminProcess {
public:
  explicit AdminProcess(std::function<void()> changed);
  ~AdminProcess();
  AdminProcess(const AdminProcess&) = delete;
  AdminProcess& operator=(const AdminProcess&) = delete;

  [[nodiscard]] AdminSession& session() noexcept { return m_session; }
  // The root helper; ANDROMEDA_FILEOPS_ADMIN points tests at a stand-in.
  [[nodiscard]] static std::string helperPath();

private:
  void readNext();
  void write(const std::string& line);

  AdminSession m_session;
  GSubprocess* m_process = nullptr;
  GDataInputStream* m_stdout = nullptr;
  GOutputStream* m_stdin = nullptr;
  GCancellable* m_cancel = nullptr;
};
