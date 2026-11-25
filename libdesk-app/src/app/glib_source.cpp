#include "app/glib_source.h"

// The context stays acquired while poll() sleeps: g_source_attach() from another thread (GDBus signal and
// method callbacks) signals the context wakeup fd only when a thread owns the context.
GlibSource::GlibSource() : m_context(g_main_context_default()) {
  if (!g_main_context_acquire(m_context)) {
    g_error("GlibSource: another thread owns the default GMainContext");
  }
}

GlibSource::~GlibSource() { g_main_context_release(m_context); }

void GlibSource::doAddPollFds(std::vector<pollfd>& fds) {
  m_fds.clear();
  m_timeoutMs = -1;
  const gboolean ready = g_main_context_prepare(m_context, &m_priority);
  gint timeout = -1;
  gint count = g_main_context_query(m_context, m_priority, &timeout, nullptr, 0);
  if (count > 0) {
    m_fds.resize(static_cast<std::size_t>(count));
    count = g_main_context_query(m_context, m_priority, &timeout, m_fds.data(), count);
    m_fds.resize(static_cast<std::size_t>(count));
  }
  m_timeoutMs = ready ? 0 : timeout;
  for (const GPollFD& fd : m_fds) {
    fds.push_back({.fd = fd.fd, .events = static_cast<short>(fd.events), .revents = 0});
  }
}

void GlibSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  for (std::size_t i = 0; i < m_fds.size(); ++i) {
    const std::size_t at = startIdx + i;
    m_fds[i].revents = at < fds.size() ? static_cast<gushort>(fds[at].revents) : static_cast<gushort>(0);
  }
  if (g_main_context_check(m_context, m_priority, m_fds.data(), static_cast<gint>(m_fds.size()))) {
    g_main_context_dispatch(m_context);
  }
}
