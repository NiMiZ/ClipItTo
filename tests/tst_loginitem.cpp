#include "LoginItem.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using clipitto::LoginItem;

class TestLoginItem : public QObject {
    Q_OBJECT

private slots:
    void writesAndRemovesTheLaunchAgent()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("Library/LaunchAgents/io.github.nimiz.ClipItTo.plist"));
        LoginItem item(path, QStringLiteral("/Applications/ClipItTo.app/Contents/MacOS/clipitto-daemon"));
        QVERIFY(!item.isEnabled());

        QString error;
        QVERIFY2(item.setEnabled(true, &error), qPrintable(error));
        QVERIFY(item.isEnabled());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QLatin1String("<string>io.github.nimiz.ClipItTo</string>")));
        QVERIFY(text.contains(QLatin1String("<string>/Applications/ClipItTo.app/Contents/MacOS/clipitto-daemon</string>")));
        QVERIFY(text.contains(QLatin1String("<key>RunAtLoad</key>\n    <true/>")));
        QVERIFY(text.contains(QLatin1String("<key>LimitLoadToSessionType</key>")));
        QVERIFY(!text.contains(QLatin1String("KeepAlive"))); // a quit ClipItTo stays quit

        QVERIFY2(item.setEnabled(true, &error), qPrintable(error)); // again: no problem
        QVERIFY2(item.setEnabled(false, &error), qPrintable(error));
        QVERIFY(!item.isEnabled());
        QVERIFY2(item.setEnabled(false, &error), qPrintable(error)); // already off
    }

    void aMovedAppIsPointedAtItsNewPlace()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("agent.plist"));
        QVERIFY(LoginItem(path, QStringLiteral("/old/clipitto-daemon")).setEnabled(true));
        QVERIFY(LoginItem(path, QStringLiteral("/new/clipitto-daemon")).setEnabled(true));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QLatin1String("/new/clipitto-daemon")));
        QVERIFY(!text.contains(QLatin1String("/old/")));
    }

    void pathsWithMarkupAreEscaped()
    {
        QTemporaryDir dir;
        LoginItem item(dir.filePath(QStringLiteral("a.plist")), QStringLiteral("/Apps/R&D <test>/clipitto-daemon"));
        QVERIFY(item.plist().contains(QLatin1String("/Apps/R&amp;D &lt;test&gt;/clipitto-daemon")));
        QVERIFY(!item.plist().contains(QLatin1String("<test>")));
    }

    void refusesWhereItCannotWrite()
    {
        QTemporaryDir dir;
        QFile blocker(dir.filePath(QStringLiteral("file")));
        QVERIFY(blocker.open(QIODevice::WriteOnly)); // a file where a folder is needed
        LoginItem item(dir.filePath(QStringLiteral("file/agent.plist")), QStringLiteral("/x"));
        QString error;
        QVERIFY(!item.setEnabled(true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!item.isEnabled());
    }

    void defaultPathIsInTheUsersLaunchAgents()
    {
        const QString path = LoginItem::defaultPlistPath();
        QVERIFY(path.endsWith(QLatin1String("/Library/LaunchAgents/io.github.nimiz.ClipItTo.plist")));
    }
};

QTEST_GUILESS_MAIN(TestLoginItem)
#include "tst_loginitem.moc"
