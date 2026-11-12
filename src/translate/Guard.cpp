// The time limit around a translation run, shared by the real engine (Translate_ct2.cpp) and tests with a fake one.
#include "Translate.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace clipitto::translate {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::translate", text);
}

std::atomic<bool> g_busy{false}; // one run at a time; released only when the run really ends

// What the worker thread hands back to the thread that waits for it.
struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::optional<QString> result;
    QString error;
    std::atomic<bool> cancelled{false};
};

} // namespace

bool guardBusy()
{
    return g_busy.load();
}

std::optional<QString> runGuarded(GuardedWork work, QString* error, int timeoutMs, GuardOutcome* outcome)
{
    const auto setOutcome = [outcome](GuardOutcome o) {
        if (outcome) {
            *outcome = o;
        }
    };
    const auto limit = std::chrono::milliseconds(std::max(timeoutMs, 1));
    // A run still going (or stuck) must not make the next one wait for ever.
    const auto until = std::chrono::steady_clock::now() + limit;
    bool expected = false;
    while (!g_busy.compare_exchange_strong(expected, true)) {
        expected = false;
        if (std::chrono::steady_clock::now() >= until) {
            setOutcome(GuardOutcome::Busy);
            if (error) {
                *error = tr("Another translation is still running. Try again in a moment.");
            }
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // The work runs on its own thread so a model that never finishes cannot hold the caller: the CPU work of a stuck
    // run can't be cancelled, but the caller gets an answer, and g_busy is released only when the worker really ends.
    auto shared = std::make_shared<Shared>();
    try {
        std::thread([shared, work = std::move(work)] {
            QString err;
            std::optional<QString> out;
            try {
                out = work(&err, shared->cancelled);
            } catch (...) {
                out.reset();
                err = tr("Translation failed.");
            }
            g_busy = false;
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->result = std::move(out);
            shared->error = err;
            shared->finished = true;
            shared->cv.notify_all();
        }).detach();
    } catch (const std::system_error&) {
        g_busy = false;
        setOutcome(GuardOutcome::StartFailed);
        if (error) {
            *error = tr("Translation could not be started.");
        }
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (!shared->cv.wait_for(lock, limit, [&shared] { return shared->finished; })) {
        shared->cancelled = true; // a run that checks between sentences ends soon and frees the next one
        setOutcome(GuardOutcome::TimedOut);
        if (error) {
            *error = tr("The translation took longer than %1 seconds and was cancelled.").arg(timeoutMs / 1000);
        }
        return std::nullopt;
    }
    setOutcome(GuardOutcome::Done);
    if (!shared->result && error) {
        *error = shared->error;
    }
    return shared->result;
}

} // namespace clipitto::translate
