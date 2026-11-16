#include "admin_process.h"

#include "core/log.h"

#include <cstdlib>
#include <gio/gio.h>

namespace {
  constexpr Logger kLog("files-admin");
} // namespace

std::string AdminProcess::helperPath() {
  const char* override = std::getenv("ANDROMEDA_FILEOPS_ADMIN");
  return override != nullptr && *override != '\0' ? override : "/usr/lib/andromeda/andromeda-fileops-admin";
}

AdminProcess::AdminProcess(std::function<void()> changed)
    : m_session([this](const std::string& line) { write(line); }, std::move(changed)) {
  const std::string helper = helperPath();
  const char* argv[] = {"pkexec", helper.c_str(), "serve", nullptr};
  GError* error = nullptr;
  m_process = g_subprocess_newv(argv, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
                                &error);
  if (m_process == nullptr) {
    kLog.warn("pkexec: {}", error->message);
    g_error_free(error);
    m_session.onExit(-1);
    return;
  }
  kLog.info("admin helper started");
  m_cancel = g_cancellable_new();
  m_stdin = G_OUTPUT_STREAM(g_object_ref(g_subprocess_get_stdin_pipe(m_process)));
  m_stdout = g_data_input_stream_new(g_subprocess_get_stdout_pipe(m_process));
  readNext();
}

AdminProcess::~AdminProcess() {
  m_session.close();
  if (m_cancel != nullptr) {
    g_cancellable_cancel(m_cancel);
  }
  if (m_stdin != nullptr) {
    // EOF on stdin ends `serve`; pkexec runs as root, so a signal from here could not stop it.
    g_output_stream_close(m_stdin, nullptr, nullptr);
    g_object_unref(m_stdin);
  }
  if (m_stdout != nullptr) {
    g_object_unref(m_stdout);
  }
  if (m_process != nullptr) {
    g_object_unref(m_process);
  }
  if (m_cancel != nullptr) {
    g_object_unref(m_cancel);
  }
}

void AdminProcess::write(const std::string& line) {
  GError* error = nullptr;
  if (g_output_stream_write_all(m_stdin, line.data(), line.size(), nullptr, nullptr, &error) == FALSE) {
    kLog.warn("admin helper write: {}", error->message);
    g_error_free(error);
  }
}

void AdminProcess::readNext() {
  g_data_input_stream_read_line_async(
      m_stdout, G_PRIORITY_DEFAULT, m_cancel,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        GError* error = nullptr;
        gsize length = 0;
        gchar* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &error);
        if (error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          g_error_free(error); // the tab closed: `data` is gone
          return;
        }
        auto* self = static_cast<AdminProcess*>(data);
        if (line != nullptr) {
          self->m_session.onLine(std::string_view(line, length));
          g_free(line);
          self->readNext();
          return;
        }
        if (error != nullptr) {
          kLog.warn("admin helper read: {}", error->message);
          g_error_free(error);
        }
        // EOF: the helper (or pkexec) ended; its exit status tells a dismissed password from a crash.
        g_subprocess_wait_async(
            self->m_process, self->m_cancel,
            [](GObject* proc, GAsyncResult* res, gpointer selfData) {
              if (g_subprocess_wait_finish(G_SUBPROCESS(proc), res, nullptr) == FALSE) {
                return; // cancelled: the tab closed
              }
              auto* me = static_cast<AdminProcess*>(selfData);
              const int status = g_subprocess_get_if_exited(G_SUBPROCESS(proc)) ? g_subprocess_get_exit_status(G_SUBPROCESS(proc)) : -1;
              kLog.info("admin helper exited {}", status);
              me->m_session.onExit(status);
            },
            self);
      },
      this);
}
