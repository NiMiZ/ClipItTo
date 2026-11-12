#pragma once

// A real Daemon on a temp directory for the perf tests: its capture pipeline, popup and database, a stand-in control
// channel, no synthetic keys, no notifications, no real keyring.

#include "ControlChannel.h"
#include "Daemon.h"
#include "FakeKeyStore.h"
#include "PopupWindow.h"
#include "db/Repository.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace perf {

class FakeControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

struct DaemonRig {
    QTemporaryDir dir;
    std::unique_ptr<clipitto::Daemon> daemon;
    std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();

    clipitto::DaemonOptions options() const
    {
        clipitto::DaemonOptions o;
        o.configPath = dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [](clipitto::PasteKeys) { return false; };
        o.notify = [](const QString&, const QString&) { return true; };
        o.keyStore = keys;
        o.matchKeyStore = keys;
        o.debugLogPath = dir.filePath(QStringLiteral("debug.log"));
        return o;
    }

    // `config` is the text of config.toml (hide_on_focus_loss is always off).
    explicit DaemonRig(const QByteArray& config = {}, bool start = true)
    {
        QFile cfg(dir.filePath(QStringLiteral("config.toml")));
        if (!cfg.open(QIODevice::WriteOnly)) {
            qFatal("cannot write the config");
        }
        cfg.write("hide_on_focus_loss = false\n" + config);
        cfg.close();
        daemon = std::make_unique<clipitto::Daemon>(options(), std::make_unique<FakeControl>());
        if (start && daemon->start() != clipitto::Daemon::StartResult::Ok) {
            qFatal("the daemon did not start");
        }
        if (start) {
            daemon->popup()->setNotify([](QWidget*, const QString&) {}); // never a modal box in a test
        }
    }

    clipitto::db::Repository& repo() const { return *daemon->repository(); }
    clipitto::ui::PopupWindow* popup() const { return daemon->popup(); }
    void ingest(const QString& text, const QString& html = QString())
    {
        emit daemon->control()->ingestRequested(text, {QStringLiteral("text/plain")}, html);
    }
};

} // namespace perf
