#include "QtClipboardBackend.h"

#include <QGuiApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>

using clipitto::capture::QtClipboardBackend;

// Runs on the offscreen platform, whose in-process clipboard emits the same
// QClipboard signals the xcb and windows platforms emit for external changes.
class TestClipboardBackend : public QObject {
    Q_OBJECT

private slots:
    void emitsTextAndMimeTypes()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();

        auto* data = new QMimeData;
        data->setText(QStringLiteral("copied text"));
        data->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
        QGuiApplication::clipboard()->setMimeData(data);

        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("copied text"));
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));
    }

    void ignoresNonText()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();

        auto* data = new QMimeData;
        data->setData(QStringLiteral("image/png"), QByteArray(16, '\0'));
        QGuiApplication::clipboard()->setMimeData(data);
        QCoreApplication::processEvents();
        QCOMPARE(captured.size(), 0);
    }

    void stopDisconnects()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        backend.stop();
        QGuiApplication::clipboard()->setText(QStringLiteral("after stop"));
        QCoreApplication::processEvents();
        QCOMPARE(captured.size(), 0);
    }
};

QTEST_MAIN(TestClipboardBackend)
#include "tst_clipboardbackend.moc"
