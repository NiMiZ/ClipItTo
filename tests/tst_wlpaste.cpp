#include "WlPasteBackend.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::capture::WlPasteBackend;

// Drives WlPasteBackend against fake-wl-paste.sh, which emulates wl-paste's CLI.
class TestWlPaste : public QObject {
    Q_OBJECT

private:
    const QString m_program = QStringLiteral(CLIPITTO_FAKE_WLPASTE);
    QTemporaryDir m_dir;

    void write(const QString& name, const QByteArray& content = {})
    {
        QFile file(m_dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content);
    }

    void clipboard(const QByteArray& text, const QByteArray& types = "text/plain;charset=utf-8\ntext/plain\n")
    {
        write(QStringLiteral("types"), types);
        write(QStringLiteral("text"), text);
    }

    QString lastText(const QSignalSpy& spy) const { return spy.isEmpty() ? QString() : spy.last().at(0).toString(); }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        for (const QString& f : QDir(m_dir.path()).entryList(QDir::Files)) {
            QFile::remove(m_dir.filePath(f));
        }
        qputenv("FAKE_WLPASTE_DIR", QFile::encodeName(m_dir.path()));
    }

    void probeDetectsDataControl()
    {
        QVERIFY(WlPasteBackend::probe(m_program));
        write(QStringLiteral("unsupported"));
        QVERIFY(!WlPasteBackend::probe(m_program));
        QVERIFY(!WlPasteBackend::probe(QStringLiteral("/nonexistent/wl-paste")));
    }

    void capturesCurrentThenLaterChanges()
    {
        clipboard("first");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();

        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("first"));
        QVERIFY(captured.last().at(1).toStringList().contains(QStringLiteral("text/plain")));

        clipboard("second\nline");
        write(QStringLiteral("event"));
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(lastText(captured), QStringLiteral("second\nline"));
    }

    void neverReadsSecrets()
    {
        clipboard("hunter2", "text/plain\nx-kde-passwordManagerHint\n");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTest::qWait(300);
        QCOMPARE(captured.size(), 0);
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("reads"))));
    }

    void skipsNonText()
    {
        clipboard("\x89PNG", "image/png\n");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTest::qWait(300);
        QCOMPARE(captured.size(), 0);
    }

    void restartsWhenTheWatcherExits()
    {
        clipboard("before");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);

        clipboard("after restart");
        write(QStringLiteral("die"));
        // Restarted after a 500 ms backoff; the new watcher reports the current content.
        QTRY_COMPARE_WITH_TIMEOUT(lastText(captured), QStringLiteral("after restart"), 5000);
    }

    void stopEndsCapture()
    {
        clipboard("one");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        backend.stop();
        clipboard("two");
        write(QStringLiteral("event"));
        QTest::qWait(200);
        QCOMPARE(captured.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestWlPaste)
#include "tst_wlpaste.moc"
