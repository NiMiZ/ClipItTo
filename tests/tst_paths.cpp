#include "Paths.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

// The config and history locations per platform (each branch tested on its own OS).
class TestPaths : public QObject {
    Q_OBJECT

private slots:
    void locations()
    {
        QTemporaryDir config;
        QTemporaryDir data;
#ifdef Q_OS_WIN
        qputenv("APPDATA", QDir::toNativeSeparators(config.path()).toLocal8Bit());
        QCOMPARE(QDir::cleanPath(clipitto::paths::configDir()), QDir(config.path()).filePath(QStringLiteral("clipitto")));
        // History stays machine-local (%LOCALAPPDATA%), not in the roaming profile.
        QVERIFY(!QDir::cleanPath(clipitto::paths::dataDir()).startsWith(QDir::cleanPath(config.path())));
#else
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));
        qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));
        QCOMPARE(clipitto::paths::configDir(), QDir(config.path()).filePath(QStringLiteral("clipitto")));
        QCOMPARE(clipitto::paths::dataDir(), QDir(data.path()).filePath(QStringLiteral("clipitto")));
#endif
        QCOMPARE(QFileInfo(clipitto::paths::configFile()).fileName(), QStringLiteral("config.toml"));
        QCOMPARE(QFileInfo(clipitto::paths::configFile()).dir().dirName(), QStringLiteral("clipitto"));
        QCOMPARE(QFileInfo(clipitto::paths::databaseFile()).fileName(), QStringLiteral("history.db"));
    }

    void ensureParentDirCreatesNestedDirectories()
    {
        QTemporaryDir root;
        const QString file = QDir(QDir(root.path()).filePath(QStringLiteral("a"))).filePath(QStringLiteral("b.db"));
        QVERIFY(clipitto::paths::ensureParentDir(file));
        QVERIFY(QFileInfo(QFileInfo(file).absolutePath()).isDir());
    }
};

QTEST_GUILESS_MAIN(TestPaths)
#include "tst_paths.moc"
