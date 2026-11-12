// Paste once (roadmap 17): a copy marked to be forgotten right after it is pasted from ClipItTo. The repository part
// (mark, snapshot, restore, secure delete), the backup rules, the popup (keys, menu, tag, the password question, pinned
// and starred copies, Undo) and the daemon (paste keys sent or not, the clipboard cleared only while it still holds the
// paste). Offscreen; no clipboard contents are printed. Not shown here (docs/untested.md): the real Windows, X11, Wayland
// and macOS clipboards and the paste keys going to a real window.
#include "Backup.h"
#include "ControlChannel.h"
#include "Daemon.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "ImageClip.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SyntheticPaste.h"
#include "TestDb.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using clipitto::Config;
using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::Entry;
using clipitto::PasteKeys;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;
using clipitto::db::Repository;

namespace {

class FakeControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

clipitto::ImageClip fakeImage(int n)
{
    clipitto::ImageClip clip;
    clip.png = QByteArray("PNG-") + QByteArray::number(n) + QByteArray(64, 'x');
    clip.thumbnail = QByteArray("THUMB-") + QByteArray::number(n);
    clip.width = 800;
    clip.height = 600;
    clip.pixelHash = QStringLiteral("hash%1").arg(n);
    return clip;
}

QByteArray pngBytes(const QColor& color)
{
    QImage picture(24, 24, QImage::Format_RGB32);
    picture.fill(color);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    picture.save(&buffer, "PNG");
    return png;
}

QImage pictureOnClipboard()
{
    QClipboard* clip = QGuiApplication::clipboard();
    QImage image = clip->image();
    if (image.isNull() && clip->mimeData()) {
        image = QImage::fromData(clip->mimeData()->data(QStringLiteral("image/png")), "PNG");
    }
    return image;
}

QAction* actionStartingWith(QMenu* menu, const QString& start)
{
    for (QAction* a : menu->actions()) {
        if (a->text().remove(QLatin1Char('&')).startsWith(start)) {
            return a;
        }
    }
    return nullptr;
}

} // namespace

class TestPasteOnce : public QObject {
    Q_OBJECT

    // ---------------- the popup ----------------
    struct PopupFixture {
        TestDb t;
        std::unique_ptr<PopupWindow> popup;
        QStringList asked; // the questions the "Delete this password copy?" prompt got
        bool answer = false;

        PopupFixture()
        {
            Config cfg;
            cfg.hideOnFocusLoss = false;
            popup = std::make_unique<PopupWindow>(*t.repo, cfg);
            popup->setOnceAskDelayMs(0);
            popup->setDeletePasswordPrompt([this](QWidget*, const QString& question) {
                asked << question;
                return answer;
            });
        }
        QStringList footerTexts() const
        {
            QStringList out;
            for (const QLabel* label : popup->findChildren<QLabel*>()) {
                out << label->text();
            }
            return out;
        }
        bool footerSays(const QString& part) const
        {
            for (const QString& text : footerTexts()) {
                if (text.contains(part)) {
                    return true;
                }
            }
            return false;
        }
    };

    // ---------------- the daemon ----------------
    QTemporaryDir m_dir;
    bool m_pasteWorks = true;
    int m_pasteCalls = 0;

    DaemonOptions options() const
    {
        DaemonOptions o;
        o.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [this](PasteKeys) {
            const_cast<TestPasteOnce*>(this)->m_pasteCalls++;
            return m_pasteWorks; // never types real keys on the test machine
        };
        return o;
    }

    void writeConfig(const QByteArray& text) const
    {
        QFile file(options().configPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
    }

    bool historyFileHolds(const QByteArray& needle) const
    {
        for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            QFile file(options().databasePath + suffix);
            if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray bytes = file.readAll();
            file.close(); // closed again at once (Windows: a file open here cannot be deleted)
            if (bytes.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    // A text copy captured the way the clipboard watcher reports it; returns it marked Paste once.
    static Entry captureOnce(Daemon& d, const QString& text, bool mark = true)
    {
        emit d.control()->ingestRequested(text, {QStringLiteral("text/plain")});
        const std::optional<qint64> id = d.repository()->findByHash(d.repository()->textHash(text));
        Entry e = d.repository()->get(*id).value();
        if (mark) {
            d.repository()->setPasteOnce(e.id, true);
            e = d.repository()->get(e.id).value();
        }
        return e;
    }

private slots:
    void init()
    {
        m_pasteWorks = true;
        m_pasteCalls = 0;
        QGuiApplication::clipboard()->clear();
        QDir(m_dir.path()).removeRecursively(); // a fresh history file and config for each test
        QDir().mkpath(m_dir.path());
    }

    // ================= repository =================
    void aCopyIsNotOnceUntilMarked()
    {
        TestDb t;
        const qint64 id = t.repo->ingest(QStringLiteral("one")).id;
        QVERIFY(!t.repo->get(id)->pasteOnce);
        t.repo->setPasteOnce(id, true);
        QVERIFY(t.repo->get(id)->pasteOnce);
        QVERIFY(t.repo->recent(10, false).front().pasteOnce);
        t.repo->setPasteOnce(id, false);
        QVERIFY(!t.repo->get(id)->pasteOnce);
    }

    void restoreBringsBackTextGroupNoteAndTimesWithoutTheMark()
    {
        TestDb t;
        const qint64 id = t.repo->ingest(QStringLiteral("kubectl get pods")).id;
        t.repo->setNote(id, QStringLiteral("pods"));
        t.repo->setGroup(id, QStringLiteral("Work/Servers"));
        t.repo->setPasteOnce(id, true);
        const Entry before = *t.repo->get(id);
        const auto snap = t.repo->snapshot(id);
        QVERIFY(snap);
        QVERIFY(t.repo->forgetNow(id));
        QVERIFY(!t.repo->get(id));
        QVERIFY(t.repo->search(QStringLiteral("kubectl")).empty());

        const qint64 back = t.repo->restore(*snap);
        const Entry after = *t.repo->get(back);
        QCOMPARE(after.content, before.content);
        QCOMPARE(after.note, QStringLiteral("pods"));
        QCOMPARE(after.group, QStringLiteral("Work/Servers"));
        QCOMPARE(after.contentHash, before.contentHash);
        QCOMPARE(after.createdAt, before.createdAt);
        QCOMPARE(after.lastUsed, before.lastUsed);
        QVERIFY(!after.pasteOnce);
        QCOMPARE(t.repo->search(QStringLiteral("kubectl")).size(), size_t(1)); // found again
    }

    void restoreBringsBackAPictureAndAPasswordBlob()
    {
        TestDb t;
        const qint64 pic = t.repo->ingestImage(fakeImage(1)).id;
        const qint64 secret = t.repo->ingestSecret(QByteArray("sealed-bytes"), QStringLiteral("secret:abc"),
                                                   QStringLiteral("KeePassXC"), QString()).id;
        const auto picSnap = *t.repo->snapshot(pic);
        const auto secretSnap = *t.repo->snapshot(secret);
        QVERIFY(t.repo->forgetNow(pic));
        QVERIFY(t.repo->forgetNow(secret));
        QCOMPARE(t.repo->count(), qint64(0));

        const qint64 pic2 = t.repo->restore(picSnap);
        QVERIFY(t.repo->get(pic2)->image);
        QCOMPARE(t.repo->imageData(pic2), fakeImage(1).png);
        QCOMPARE(t.repo->get(pic2)->thumbnail, fakeImage(1).thumbnail);
        const qint64 secret2 = t.repo->restore(secretSnap);
        QVERIFY(t.repo->get(secret2)->secret);
        QCOMPARE(t.repo->sealedSecret(secret2), QByteArray("sealed-bytes"));
        QCOMPARE(t.repo->get(secret2)->note, QStringLiteral("KeePassXC"));
    }

    void restoreLeavesACopyMadeMeanwhileAlone()
    {
        TestDb t;
        const qint64 id = t.repo->ingest(QStringLiteral("again")).id;
        const auto snap = *t.repo->snapshot(id);
        QVERIFY(t.repo->forgetNow(id));
        const qint64 newer = t.repo->ingest(QStringLiteral("again")).id; // copied again within the ten seconds
        QCOMPARE(t.repo->restore(snap), newer);
        QCOMPARE(t.repo->count(), qint64(1));
        QVERIFY(!t.repo->forgetNow(9999)); // no such copy
    }

    void aForgottenCopyIsNotLeftInTheFileOrTheLog()
    {
        const QString path = m_dir.filePath(QStringLiteral("secure.db"));
        const auto holds = [&](const QByteArray& needle) {
            for (const QString& name : {path, path + QStringLiteral("-wal")}) {
                QFile f(name);
                if (f.open(QIODevice::ReadOnly)) {
                    const bool has = f.readAll().contains(needle);
                    f.close();
                    if (has) {
                        return true;
                    }
                }
            }
            return false;
        };
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            const qint64 once = repo.ingest(QStringLiteral("zqxw-once-mkr")).id;
            repo.ingest(QStringLiteral("zqxw-kept-mkr"));
            QVERIFY(holds("zqxw-once-mkr"));
            QVERIFY(repo.forgetNow(once));
            QVERIFY(!holds("zqxw-once-mkr"));
            QVERIFY(holds("zqxw-kept-mkr"));
            QCOMPARE(repo.search(QStringLiteral("zqxw")).size(), size_t(1)); // and gone from the search index
        }
    }

    // ================= backup =================
    void aBackupHasNoPasteOnceCopiesAndAnImportIgnoresThem()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("kept text"));
        const qint64 once = a.repo->ingest(QStringLiteral("one-time text")).id;
        a.repo->setPasteOnce(once, true);
        const qint64 pic = a.repo->ingestImage(fakeImage(2)).id;
        a.repo->setPasteOnce(pic, true);
        const QString file = m_dir.filePath(QStringLiteral("backup.json"));
        QString error;
        clipitto::backup::Parts parts;
        parts.copies = true;
        QVERIFY2(clipitto::backup::exportTo(file, *a.repo, Config{}, nullptr, parts, {}, &error), qPrintable(error));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray bytes = f.readAll();
        f.close();
        QVERIFY(bytes.contains("kept text"));
        QVERIFY(!bytes.contains("one-time text"));
        QCOMPARE(clipitto::backup::inspect(file).copies, 1);

        // A file that does carry a copy flagged "once" (this program never writes one): skipped on import.
        QJsonDocument doc = QJsonDocument::fromJson(bytes);
        QJsonObject root = doc.object();
        QJsonObject copies = root.value(QStringLiteral("copies")).toObject();
        QJsonArray items = copies.value(QStringLiteral("items")).toArray();
        QJsonObject flagged;
        flagged[QStringLiteral("content")] = QStringLiteral("flagged once");
        flagged[QStringLiteral("once")] = true;
        items.append(flagged);
        copies[QStringLiteral("items")] = items;
        root[QStringLiteral("copies")] = copies;
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();
        TestDb b;
        const auto report = clipitto::backup::importFrom(file, parts, *b.repo, nullptr, {}, &error);
        QVERIFY2(report, qPrintable(error));
        QCOMPARE(b.repo->count(), qint64(1));
        QVERIFY(b.repo->search(QStringLiteral("flagged")).empty());
    }

    // ================= popup =================
    void aPastedTextCopyIsForgotten()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("one-time code 481516")).id;
        f.t.repo->ingest(QStringLiteral("another"));
        f.popup->consumePasteOnce(id);
        QVERIFY(!f.t.repo->get(id));
        QVERIFY(f.t.repo->search(QStringLiteral("481516")).empty()); // never found by search after use
        QVERIFY(f.popup->undoPending());
        QVERIFY(f.asked.isEmpty());
    }

    void aGroupedCopyIsDeletedToo()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("in a group")).id;
        f.t.repo->setGroup(id, QStringLiteral("Work"));
        f.popup->consumePasteOnce(id);
        QVERIFY(!f.t.repo->get(id));
    }

    void aPictureIsDeleted()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingestImage(fakeImage(3)).id;
        f.popup->consumePasteOnce(id);
        QVERIFY(!f.t.repo->get(id));
        QVERIFY(f.t.repo->imageData(id).isEmpty());
    }

    void pinnedAndStarredCopiesAreKeptAndLoseTheMark()
    {
        PopupFixture f;
        const qint64 pinned = f.t.repo->ingest(QStringLiteral("pinned one")).id;
        const qint64 starred = f.t.repo->ingest(QStringLiteral("starred one")).id;
        f.t.repo->setPinned(pinned, true);
        f.t.repo->setFavorite(starred, true);
        f.t.repo->setPasteOnce(pinned, true);
        f.t.repo->setPasteOnce(starred, true);

        f.popup->consumePasteOnce(pinned);
        QVERIFY(f.t.repo->get(pinned));
        QVERIFY(!f.t.repo->get(pinned)->pasteOnce);
        QVERIFY(f.footerSays(QStringLiteral("Pinned copies are not deleted")));
        f.popup->consumePasteOnce(starred);
        QVERIFY(f.t.repo->get(starred));
        QVERIFY(!f.t.repo->get(starred)->pasteOnce);
        QVERIFY(f.footerSays(QStringLiteral("Starred copies are not deleted")));
        QVERIFY(!f.popup->undoPending()); // nothing was deleted
    }

    void aPasswordCopyIsDeletedOnlyAfterYes_andAskedEveryTime()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:1"),
                                                 QStringLiteral("bank"), QString()).id;
        f.t.repo->setPasteOnce(id, true);

        f.answer = false; // No: kept
        f.popup->consumePasteOnce(id);
        QTRY_COMPARE(f.asked.size(), 1);
        QVERIFY(f.asked.first().contains(QStringLiteral("Delete this password copy?")));
        QVERIFY(f.t.repo->get(id));
        QVERIFY(!f.popup->undoPending());
        QVERIFY(!f.t.repo->get(id)->pasteOnce); // kept: it is not one-time any more

        f.t.repo->setPasteOnce(id, true); // marked again, pasted again: asked again
        f.popup->consumePasteOnce(id);
        QTRY_COMPARE(f.asked.size(), 2);
        QVERIFY(f.t.repo->get(id));

        f.answer = true; // Yes: deleted (with an Undo)
        f.t.repo->setPasteOnce(id, true);
        f.popup->consumePasteOnce(id);
        QTRY_COMPARE(f.asked.size(), 3);
        QVERIFY(!f.t.repo->get(id));
        QVERIFY(f.popup->undoPending());
        QVERIFY(f.popup->undoPasteOnce()); // and back: the sealed copy, never in clear
        const auto back = f.t.repo->findByHash(QStringLiteral("secret:1"));
        QVERIFY(back);
        QCOMPARE(f.t.repo->sealedSecret(*back), QByteArray("sealed"));
    }

    void aPinnedPasswordCopyIsNotAskedAboutBecauseNothingIsDeleted()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:2"), QString(), QString()).id;
        f.t.repo->setPinned(id, true);
        f.t.repo->setPasteOnce(id, true);
        f.popup->consumePasteOnce(id);
        QTest::qWait(50);
        QVERIFY(f.asked.isEmpty());
        QVERIFY(f.t.repo->get(id));
        QVERIFY(!f.t.repo->get(id)->pasteOnce);
    }

    void undoPutsTheCopyBackExactly()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("undo me")).id;
        f.t.repo->setNote(id, QStringLiteral("a note"));
        f.t.repo->setGroup(id, QStringLiteral("Work"));
        f.t.repo->setPasteOnce(id, true);
        f.popup->consumePasteOnce(id);
        QVERIFY(!f.t.repo->get(id));
        QVERIFY(f.popup->undoButton()->isHidden() == false || !f.popup->isVisible()); // shown with the popup
        f.popup->undoButton()->click();
        QVERIFY(!f.popup->undoPending());
        const auto back = f.t.repo->findByHash(f.t.repo->textHash(QStringLiteral("undo me")));
        QVERIFY(back);
        const Entry e = *f.t.repo->get(*back);
        QCOMPARE(e.note, QStringLiteral("a note"));
        QCOMPARE(e.group, QStringLiteral("Work"));
        QVERIFY(!e.pasteOnce); // the mark is removed
        QVERIFY(!f.popup->undoPasteOnce()); // once
    }

    void undoOfAPictureAndItEndsAfterTheTime()
    {
        PopupFixture f;
        f.popup->setUndoMs(60);
        const qint64 id = f.t.repo->ingestImage(fakeImage(4)).id;
        f.popup->consumePasteOnce(id);
        QVERIFY(f.popup->undoPending());
        QTRY_VERIFY_WITH_TIMEOUT(!f.popup->undoPending(), 10000); // the copy is gone for good
        QVERIFY(!f.popup->undoPasteOnce());
        QVERIFY(f.t.repo->findByHash(QStringLiteral("image:hash4")) == std::nullopt);

        f.popup->setUndoMs(PopupWindow::kUndoMs);
        const qint64 id2 = f.t.repo->ingestImage(fakeImage(5)).id;
        f.popup->consumePasteOnce(id2);
        QVERIFY(f.popup->undoPasteOnce());
        const auto back = f.t.repo->findByHash(QStringLiteral("image:hash5"));
        QVERIFY(back);
        QCOMPARE(f.t.repo->imageData(*back), fakeImage(5).png);
    }

    void ctrlShiftEnterPastesAndForgetsFromTheListAndTheSearchBox()
    {
        PopupFixture f;
        f.t.repo->ingest(QStringLiteral("alpha"));
        f.t.repo->ingest(QStringLiteral("beta"));
        f.popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(f.popup.get()));
        QSignalSpy once(f.popup.get(), &PopupWindow::onceActivated);
        QSignalSpy plain(f.popup.get(), &PopupWindow::entryActivated);
        f.popup->view()->setFocus();
        QTest::keyClick(f.popup->view(), Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(once.size(), 1);
        QCOMPARE(plain.size(), 0);
        QCOMPARE(once.at(0).at(0).value<Entry>().content, QStringLiteral("beta")); // the latest copy: not marked before
        QVERIFY(!once.at(0).at(0).value<Entry>().pasteOnce);

        f.popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(f.popup.get()));
        f.popup->searchBar()->setFocus();
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(once.size(), 2);
        QCOMPARE(plain.size(), 0);
    }

    void enterOnAMarkedCopyIsTheOrdinaryActivationWithTheMarkOnTheEntry()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("marked")).id;
        f.t.repo->setPasteOnce(id, true);
        f.popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(f.popup.get()));
        QSignalSpy plain(f.popup.get(), &PopupWindow::entryActivated);
        f.popup->view()->setFocus();
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QCOMPARE(plain.size(), 1);
        QVERIFY(plain.at(0).at(0).value<Entry>().pasteOnce);
    }

    void theMenuTogglesTheMark()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("menu copy")).id;
        f.popup->refresh();
        const auto menuFor = [&](std::unique_ptr<QMenu>& owner) {
            owner.reset(f.popup->createContextMenu(*f.t.repo->get(id), f.popup.get()));
            return actionStartingWith(owner.get(), QStringLiteral("Paste once"));
        };
        std::unique_ptr<QMenu> owner;
        QAction* once = menuFor(owner);
        QVERIFY(once);
        QVERIFY(once->isCheckable());
        QVERIFY(!once->isChecked());
        once->trigger();
        QVERIFY(f.t.repo->get(id)->pasteOnce);
        once = menuFor(owner);
        QVERIFY(once->isChecked()); // the toggle shows it, and
        once->trigger();
        QVERIFY(!f.t.repo->get(id)->pasteOnce); // can be unmarked

        // The Shift+Enter menu gets the same toggle.
        QMenu shiftEnter;
        f.popup->addPasteOnceAction(&shiftEnter, *f.t.repo->get(id));
        QAction* again = actionStartingWith(&shiftEnter, QStringLiteral("Paste once"));
        QVERIFY(again);
        again->trigger();
        QVERIFY(f.t.repo->get(id)->pasteOnce);
        // Passwords and pictures have it too.
        const qint64 secret = f.t.repo->ingestSecret(QByteArray("s"), QStringLiteral("secret:3"), QString(), QString()).id;
        std::unique_ptr<QMenu> secretMenu(f.popup->createContextMenu(*f.t.repo->get(secret), f.popup.get()));
        QVERIFY(actionStartingWith(secretMenu.get(), QStringLiteral("Paste once")));
        const qint64 pic = f.t.repo->ingestImage(fakeImage(6)).id;
        std::unique_ptr<QMenu> picMenu(f.popup->createContextMenu(*f.t.repo->get(pic), f.popup.get()));
        QVERIFY(actionStartingWith(picMenu.get(), QStringLiteral("Paste once")));
    }

    void theRowShowsAOnceTag()
    {
        PopupFixture f;
        const qint64 id = f.t.repo->ingest(QStringLiteral("tagged copy")).id;
        f.popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(f.popup.get()));
        const QModelIndex row = f.popup->model()->findEntry(id);
        QVERIFY(row.isValid());
        QVERIFY(!row.data(HistoryModel::PasteOnceRole).toBool());
        const QImage plain = f.popup->view()->viewport()->grab().toImage();
        f.t.repo->setPasteOnce(id, true);
        f.popup->refresh();
        QVERIFY(f.popup->model()->findEntry(id).data(HistoryModel::PasteOnceRole).toBool());
        const QImage tagged = f.popup->view()->viewport()->grab().toImage();
        QVERIFY(plain != tagged); // the small "once" tag is painted
    }

    // ================= the daemon =================
    void aMarkedCopyIsForgottenAfterItsPasteAndTheClipboardIsCleared()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.popup()->setOnceAskDelayMs(0);
        const Entry e = captureOnce(d, QStringLiteral("481516-daemon-once"));
        QVERIFY(e.pasteOnce);
        emit d.popup()->entryActivated(e);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("481516-daemon-once"));
        QTRY_VERIFY(!d.repository()->get(e.id)); // after the paste keys were sent
        QCOMPARE(m_pasteCalls, 1);
        QVERIFY(d.popup()->undoPending());
        // The clipboard is emptied after the seconds, and that is not a copy.
        QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().isEmpty(), 8000);
        QCOMPARE(d.repository()->count(), qint64(0));
        emit d.control()->ingestRequested(QStringLiteral("the next copy"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->count(), qint64(1));
    }

    void ctrlShiftEnterPastesAndForgetsACopyThatWasNotMarked()
    {
        writeConfig("paste_once_clear_seconds = 0\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Entry e = captureOnce(d, QStringLiteral("not marked before"), false);
        QVERIFY(!e.pasteOnce);
        emit d.popup()->onceActivated(e);
        QTRY_VERIFY(!d.repository()->get(e.id));
        // Setting 0: it stays on the clipboard.
        QTest::qWait(1500);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("not marked before"));
    }

    void anOrdinaryPasteOfAnUnmarkedCopyKeepsIt()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Entry e = captureOnce(d, QStringLiteral("ordinary"), false);
        emit d.popup()->entryActivated(e);
        QTRY_COMPARE(m_pasteCalls, 1);
        QVERIFY(d.repository()->get(e.id));
        QTest::qWait(300);
        QVERIFY(d.repository()->get(e.id));
    }

    void ifThePasteKeysAreNotSentTheCopyAndTheClipboardStay()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        m_pasteWorks = false; // blocked or unsupported
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Entry e = captureOnce(d, QStringLiteral("kept-when-blocked"));
        emit d.popup()->entryActivated(e);
        QTRY_COMPARE(m_pasteCalls, 1);
        QTest::qWait(1800);
        QVERIFY(d.repository()->get(e.id));
        QVERIFY(d.repository()->get(e.id)->pasteOnce); // still marked
        QVERIFY(!d.popup()->undoPending());
        // The user may paste by hand: the clipboard is not emptied behind their back.
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("kept-when-blocked"));
    }

    void withAutomaticPasteOffNothingIsDeleted()
    {
        writeConfig("paste_on_enter = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Entry e = captureOnce(d, QStringLiteral("no-auto-paste"));
        emit d.popup()->entryActivated(e);
        QTest::qWait(400);
        QCOMPARE(m_pasteCalls, 0);
        QVERIFY(d.repository()->get(e.id));
    }

    void aNewerCopyIsNeverCleared()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Entry e = captureOnce(d, QStringLiteral("older-once-text"));
        emit d.popup()->entryActivated(e);
        QTRY_VERIFY(!d.repository()->get(e.id));
        QGuiApplication::clipboard()->setText(QStringLiteral("copied right after"));
        QTest::qWait(1800);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copied right after"));
    }

    void aPendingClearHappensWhenTheDaemonQuits()
    {
        writeConfig("paste_once_clear_seconds = 60\n");
        {
            Daemon d(options(), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            const Entry e = captureOnce(d, QStringLiteral("quit-once-text"));
            emit d.popup()->entryActivated(e);
            QTRY_VERIFY(!d.repository()->get(e.id));
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("quit-once-text"));
        }
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
    }

    void aPasswordCopyIsPastedThenAskedAndNoKeepsIt()
    {
        writeConfig("paste_once_clear_seconds = 0\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.popup()->setOnceAskDelayMs(0);
        QStringList asked;
        bool yes = false;
        d.popup()->setDeletePasswordPrompt([&](QWidget*, const QString& q) {
            asked << q;
            return yes;
        });
        // A password copy that reaches the clipboard needs the vault; the daemon test of the vault does that. Here the
        // copy is sealed with an open vault so that reveal works.
        d.passwords()->setKdfCost({64, 1});
        std::optional<bool> ok;
        d.passwords()->useWithoutMasterPassword([&ok](bool r) { ok = r; });
        QVERIFY(QTest::qWaitFor([&ok] { return ok.has_value(); }, 10000) && *ok);
        const auto& vault = d.passwords()->vault();
        const qint64 id = d.repository()->ingestSecret(vault.seal("pw-once-Zq81"), vault.match("pw-once-Zq81"),
                                                       QStringLiteral("bank"), QString()).id;
        d.repository()->setPasteOnce(id, true);
        const Entry e = *d.repository()->get(id);

        emit d.popup()->entryActivated(e);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("pw-once-Zq81")); // pasted first
        QTRY_COMPARE(asked.size(), 1); // then asked
        QVERIFY(d.repository()->get(id)); // No: kept
        QVERIFY(!d.repository()->get(id)->pasteOnce);

        d.repository()->setPasteOnce(id, true);
        yes = true;
        emit d.popup()->onceActivated(*d.repository()->get(id)); // asked every time, also for Ctrl+Shift+Enter
        QTRY_COMPARE(asked.size(), 2);
        QTRY_VERIFY(!d.repository()->get(id)); // Yes: deleted
        QCOMPARE(d.repository()->countSecrets(), qint64(0));
    }

    void aPictureIsPastedAndForgottenAndTheClipboardClearedWhenUnchanged()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestImageRequested(pngBytes(Qt::green), QStringLiteral("image/png"),
                                               {QStringLiteral("image/png")});
        QTRY_COMPARE(d.repository()->count(), qint64(1));
        Entry e = d.repository()->allEntries().front();
        QVERIFY(e.image);
        d.repository()->setPasteOnce(e.id, true);
        e = *d.repository()->get(e.id);
        emit d.popup()->entryActivated(e);
        QVERIFY(!pictureOnClipboard().isNull());
        QTRY_VERIFY(!d.repository()->get(e.id));
        QTRY_VERIFY_WITH_TIMEOUT(pictureOnClipboard().isNull(), 8000);
        QCOMPARE(d.repository()->count(), qint64(0)); // the clear is not a copy
    }

    void aPictureIsNotClearedWhenSomethingNewerIsOnTheClipboard()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestImageRequested(pngBytes(Qt::red), QStringLiteral("image/png"),
                                               {QStringLiteral("image/png")});
        QTRY_COMPARE(d.repository()->count(), qint64(1));
        Entry e = d.repository()->allEntries().front();
        emit d.popup()->onceActivated(e);
        QTRY_VERIFY(!d.repository()->get(e.id));
        QGuiApplication::clipboard()->setText(QStringLiteral("text copied after"));
        QTest::qWait(1800);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("text copied after"));
    }

    void theMarkSurvivesARestartAndTheForgottenCopyIsNotInTheFile()
    {
        qint64 id = 0;
        {
            Daemon d(options(), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            id = captureOnce(d, QStringLiteral("zqxw-survives-mkr")).id;
            captureOnce(d, QStringLiteral("zqxw-forgotten-mkr"));
            QVERIFY(historyFileHolds("zqxw-survives-mkr"));
        }
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const auto back = d.repository()->get(id);
        QVERIFY(back); // stored like any other copy
        QVERIFY(back->pasteOnce); // and still marked after the restart
        // Using the other one removes it from the file and the log (secure delete + checkpoint).
        const auto other = d.repository()->findByHash(d.repository()->textHash(QStringLiteral("zqxw-forgotten-mkr")));
        QVERIFY(other);
        emit d.popup()->onceActivated(*d.repository()->get(*other));
        QTRY_VERIFY(!d.repository()->get(*other));
        QVERIFY(!historyFileHolds("zqxw-forgotten-mkr"));
        QVERIFY(historyFileHolds("zqxw-survives-mkr"));
        // The row shows its tag.
        QVERIFY(d.popup()->model()->findEntry(id).data(HistoryModel::PasteOnceRole).toBool());
    }

    void aMarkedCopyIsNotInAnExportedBackupOfTheDaemonsHistory()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        captureOnce(d, QStringLiteral("backup-kept-text"), false);
        captureOnce(d, QStringLiteral("backup-once-text"));
        QString error;
        clipitto::backup::Parts parts;
        parts.copies = true;
        const QString file = m_dir.filePath(QStringLiteral("daemon-backup.json"));
        QVERIFY2(clipitto::backup::exportTo(file, *d.repository(), d.config(), d.passwords(), parts, {}, &error),
                 qPrintable(error));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        f.close();
        QVERIFY(bytes.contains("backup-kept-text"));
        QVERIFY(!bytes.contains("backup-once-text"));
    }
};

QTEST_MAIN(TestPasteOnce)
#include "tst_pasteonce.moc"
