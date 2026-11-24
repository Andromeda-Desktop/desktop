#include "admin_session.h"

#include "i18n/i18n.h"


#include <utility>

AdminSession::AdminSession(std::function<void(const std::string&)> write, std::function<void()> changed)
    : m_write(std::move(write)), m_changed(std::move(changed)) {}

bool AdminSession::transition(Phase to, std::string failure) {
  const auto allowed = [&] {
    switch (m_phase) {
    case Phase::Starting:
      return to == Phase::Ready || to == Phase::Failed || to == Phase::Closed;
    case Phase::Ready:
      return to == Phase::Busy || to == Phase::Failed || to == Phase::Closed;
    case Phase::Busy:
      return to == Phase::Ready || to == Phase::Failed || to == Phase::Closed;
    case Phase::Failed:
      return to == Phase::Closed;
    case Phase::Closed:
      return false;
    }
    return false;
  }();
  if (!allowed) {
    return false;
  }
  m_phase = to;
  m_failure = to == Phase::Failed ? std::move(failure) : std::string();
  if (m_changed) {
    m_changed();
  }
  return true;
}

void AdminSession::request(nlohmann::json op, Reply reply) {
  if (m_phase == Phase::Failed || m_phase == Phase::Closed) {
    reply({{"ok", false}, {"error", m_phase == Phase::Failed ? m_failure : std::string(i18n::tr("files.admin.tab-closed"))}});
    return;
  }
  m_queue.push_back({.op = std::move(op), .reply = std::move(reply)});
  pump();
}

void AdminSession::pump() {
  if (m_phase != Phase::Ready || m_queue.empty()) {
    return;
  }
  Pending next = std::move(m_queue.front());
  m_queue.pop_front();
  const std::uint64_t id = m_nextId++;
  next.op["id"] = id;
  m_inFlight = InFlight{.id = id, .reply = std::move(next.reply)};
  transition(Phase::Busy);
  m_write(next.op.dump() + "\n");
}

void AdminSession::onLine(std::string_view line) {
  const nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
  if (msg.is_discarded() || !msg.is_object()) {
    fail(i18n::tr("files.admin.helper-unreadable"));
    return;
  }
  if (m_phase == Phase::Starting) {
    if (msg.value("ready", false)) {
      transition(Phase::Ready);
      pump();
    } else {
      fail(i18n::tr("files.admin.helper-not-started"));
    }
    return;
  }
  if (m_phase != Phase::Busy || !m_inFlight.has_value() || msg.value("id", std::uint64_t{0}) != m_inFlight->id) {
    if (m_phase == Phase::Ready || m_phase == Phase::Busy) {
      fail(i18n::tr("files.admin.helper-out-of-order"));
    }
    return;
  }
  Reply reply = std::move(m_inFlight->reply);
  m_inFlight.reset();
  transition(Phase::Ready);
  reply(msg);
  pump();
}

void AdminSession::onExit(int status) {
  if (m_phase == Phase::Closed || m_phase == Phase::Failed) {
    return;
  }
  if (m_phase == Phase::Starting && status == 126) {
    fail(i18n::tr("files.admin.no-password"));
  } else if (m_phase == Phase::Starting && status == 127) {
    fail(i18n::tr("files.admin.not-allowed"));
  } else {
    fail(i18n::tr("files.admin.helper-stopped"));
  }
}

void AdminSession::fail(std::string why) {
  if (transition(Phase::Failed, std::move(why))) {
    drain(m_failure);
  }
}

void AdminSession::close() {
  const bool wasFailed = m_phase == Phase::Failed;
  if (transition(Phase::Closed) && !wasFailed) {
    drain(i18n::tr("files.admin.tab-closed"));
  }
}

void AdminSession::drain(const std::string& error) {
  std::deque<Reply> replies;
  if (m_inFlight.has_value()) {
    replies.push_back(std::move(m_inFlight->reply));
    m_inFlight.reset();
  }
  for (Pending& p : m_queue) {
    replies.push_back(std::move(p.reply));
  }
  m_queue.clear();
  const nlohmann::json reply{{"ok", false}, {"error", error}};
  for (Reply& r : replies) {
    r(reply);
  }
}
