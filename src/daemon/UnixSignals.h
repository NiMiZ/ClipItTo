#pragma once

#include <QObject>

#include <initializer_list>
#include <vector>

class QSocketNotifier;

namespace clipitto {

// Delivers POSIX signals through the Qt event loop (self-pipe trick).
// Only one instance may exist per process.
class UnixSignals : public QObject {
    Q_OBJECT

public:
    UnixSignals(std::initializer_list<int> signos, QObject* parent = nullptr);
    ~UnixSignals() override;

signals:
    void received(int signo);

private:
    static void handler(int signo);
    void readPending();

    static int s_fds[2];
    std::vector<int> m_signos;
    QSocketNotifier* m_notifier = nullptr;
};

} // namespace clipitto
