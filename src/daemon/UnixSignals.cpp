#include "UnixSignals.h"

#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace clipitto {

int UnixSignals::s_fds[2] = {-1, -1};

UnixSignals::UnixSignals(std::initializer_list<int> signos, QObject* parent) : QObject(parent), m_signos(signos)
{
    Q_ASSERT(s_fds[0] == -1);
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s_fds) != 0) {
        qWarning("UnixSignals: socketpair failed; signals will use default handling");
        return;
    }
    ::fcntl(s_fds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(s_fds[1], F_SETFL, O_NONBLOCK);

    m_notifier = new QSocketNotifier(s_fds[1], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &UnixSignals::readPending);

    struct sigaction action = {};
    action.sa_handler = &UnixSignals::handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signo : m_signos) {
        ::sigaction(signo, &action, nullptr);
    }
}

UnixSignals::~UnixSignals()
{
    for (const int signo : m_signos) {
        ::signal(signo, SIG_DFL);
    }
    if (s_fds[0] != -1) {
        ::close(s_fds[0]);
        ::close(s_fds[1]);
        s_fds[0] = s_fds[1] = -1;
    }
}

void UnixSignals::handler(int signo)
{
    const unsigned char byte = static_cast<unsigned char>(signo);
    [[maybe_unused]] const auto written = ::write(s_fds[0], &byte, 1); // async-signal-safe
}

void UnixSignals::readPending()
{
    unsigned char byte = 0;
    while (::read(s_fds[1], &byte, 1) == 1) {
        emit received(byte);
    }
}

} // namespace clipitto
