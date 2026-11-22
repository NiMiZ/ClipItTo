// "Show similar copies" and "Keep this one, delete the others" (roadmap 22): the normalising and the closeness rule
// (Similar.h), the repository page it reads, and the popup view (protected copies, passwords and pictures left out,
// the confirmation, Undo, the time budget, no GUI stall, nothing logged but counts). Offscreen.
#include "HistoryModel.h"
#include "HistoryView.h"
#include "ImageClip.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "Similar.h"
#include "StallWatchdog.h"
#include "TestDb.h"
#include "db/Repository.h"

#include <QAction>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QTest>
#include <QToolButton>

using clipitto::Config;
using clipitto::Entry;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;
namespace similar = clipitto::similar;

namespace {

QStringList g_log;

void captureLog(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_log << message;
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

const QString kBase = QStringLiteral("The quick brown fox jumps over the lazy dog, then naps in the warm sun all day.");

struct Fixture {
    TestDb t;
    std::unique_ptr<PopupWindow> popup;
    QStringList questions;
    bool answer = true;
    QStringList messages;

    Fixture()
    {
        Config cfg;
        cfg.hideOnFocusLoss = false;
        popup = std::make_unique<PopupWindow>(*t.repo, cfg);
        popup->setConfirmPrompt([this](QWidget*, const QString& q) {
            questions << q;
            return answer;
        });
        popup->setNotify([this](QWidget*, const QString& m) { messages << m; });
    }
    qint64 add(const QString& text) { return t.repo->ingest(text).id; }
    bool waitDone() const
    {
        return QTest::qWaitFor([this] { return !popup->similarScanning(); }, 20000);
    }
    QList<qint64> shownIds() const
    {
        QList<qint64> ids;
        const HistoryModel* model = popup->model();
        for (int r = 0; r < model->rowCount(); ++r) {
            const QModelIndex section = model->index(r, 0);
            if (const Entry* e = model->entryAt(section)) {
                ids << e->id;
            }
            for (int c = 0; c < model->rowCount(section); ++c) {
                if (const Entry* e = model->entryAt(model->index(c, 0, section))) {
                    ids << e->id;
                }
            }
        }
        return ids;
    }
    QString footer() const
    {
        QString all;
        for (const QLabel* label : popup->findChildren<QLabel*>()) {
            all += label->text() + QLatin1Char('\n');
        }
        return all;
    }
};

} // namespace

class TestSimilar : public QObject {
    Q_OBJECT

private slots:
    // ---------------- normalising ----------------
    void normaliseTrimsCollapsesFoldsAndUnifiesLineEndings_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("trim") << "  hello  " << "hello";
        QTest::newRow("runs") << "a   b\t\tc" << "a b c";
        QTest::newRow("case") << "Hello WORLD" << "hello world";
        QTest::newRow("crlf") << "one\r\ntwo\rthree\nfour" << "one two three four";
        QTest::newRow("trailing newline") << "text\n" << "text";
        QTest::newRow("nbsp") << QString(QStringLiteral("a  b")) << "a b";
        QTest::newRow("umlaut") << QString(QStringLiteral("\u00c4\u00d6X")) << QString(QStringLiteral("\u00e4\u00f6x"));
        QTest::newRow("only space") << " \n\t " << "";
    }
    void normaliseTrimsCollapsesFoldsAndUnifiesLineEndings()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(similar::normalise(in), out);
    }

    void normaliseCutsAtTheCharacterLimit()
    {
        QCOMPARE(similar::normalise(QString(similar::kMaxChars + 500, QLatin1Char('a'))).size(), qsizetype(similar::kMaxChars));
    }

    // ---------------- closeness ----------------
    void sameAfterNormalisingMatches()
    {
        const similar::Reference ref(kBase);
        QVERIFY(ref.matches(QStringLiteral("  ") + kBase.toUpper() + QStringLiteral("\r\n")));
        QVERIFY(ref.matches(QString(kBase).replace(QLatin1Char(' '), QStringLiteral("  "))));
    }

    void nearMatchWithinTenPercentMatches()
    {
        const similar::Reference ref(kBase); // 80 characters: up to 8 may differ
        QString changed = kBase;
        changed.replace(QStringLiteral("quick"), QStringLiteral("slow!")); // 5 substitutions
        QVERIFY(ref.matches(changed));
        QVERIFY(ref.matches(kBase + QStringLiteral(" Yes."))); // 5 inserted
        QVERIFY(ref.matches(kBase.left(kBase.size() - 6))); // 6 deleted
    }

    void farTextDoesNotMatch()
    {
        const similar::Reference ref(kBase);
        QVERIFY(!ref.matches(QStringLiteral("Something entirely different about invoices.")));
        QVERIFY(!ref.matches(kBase.left(kBase.size() / 2))); // lengths too far apart
        QString many = kBase;
        many.replace(QStringLiteral("quick brown fox"), QStringLiteral("slow yellow cat")); // 12+ substitutions
        QVERIFY(!ref.matches(many));
        QVERIFY(!ref.matches(QString()));
        QVERIFY(!similar::Reference(QStringLiteral("  ")).matches(QStringLiteral(" ")));
    }

    void shortTextsMatchOnlyWhenEqual()
    {
        const similar::Reference ref(QStringLiteral("ab"));
        QVERIFY(ref.matches(QStringLiteral(" AB ")));
        QVERIFY(!ref.matches(QStringLiteral("ac")));
    }

    void boundedDistanceIsExactWithinTheLimit()
    {
        QCOMPARE(similar::boundedDistance(QStringLiteral("kitten"), QStringLiteral("sitting"), 5), 3);
        QCOMPARE(similar::boundedDistance(QStringLiteral("kitten"), QStringLiteral("sitting"), 2), 3); // limit + 1
        QCOMPARE(similar::boundedDistance(QStringLiteral("abc"), QStringLiteral("abc"), 0), 0);
        QCOMPARE(similar::boundedDistance(QStringLiteral("abc"), QStringLiteral("abcd"), 3), 1);
    }

    // ---------------- the repository page ----------------
    void thePageLeavesOutPasswordsAndPicturesAndCutsTheText()
    {
        TestDb t;
        const qint64 a = t.repo->ingest(QString(300, QLatin1Char('x'))).id;
        t.repo->ingestImage(fakeImage(1));
        t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:1"), QString(), QString());
        const qint64 b = t.repo->ingest(QStringLiteral("newer")).id;
        const auto page = t.repo->similarPage(0, 10, 100);
        QCOMPARE(page.size(), size_t(2));
        QCOMPARE(page[0].id, b); // newest first
        QCOMPARE(page[1].id, a);
        QCOMPARE(page[1].content.size(), qsizetype(100));
        QCOMPARE(t.repo->similarPage(1, 10, 100).size(), size_t(1)); // offset
    }

    // ---------------- the popup view ----------------
    void showsOnlyTheSameAndCloseCopies()
    {
        Fixture f;
        const qint64 far = f.add(QStringLiteral("Totally unrelated: the meeting is at noon on Thursday in room 4."));
        const qint64 same = f.add(QStringLiteral("  ") + kBase.toUpper() + QStringLiteral("\n"));
        const qint64 near = f.add(kBase + QStringLiteral(" Yes."));
        const qint64 ref = f.add(kBase);
        f.popup->showSimilar(ref);
        QVERIFY(f.popup->similarActive());
        QVERIFY(f.waitDone());
        const QList<qint64> ids = f.shownIds();
        QVERIFY(ids.contains(ref));
        QVERIFY(ids.contains(same));
        QVERIFY(ids.contains(near));
        QVERIFY(!ids.contains(far));
        QCOMPARE(ids.size(), 3);
        QVERIFY(f.footer().contains(QStringLiteral("2 similar copies")));
        QVERIFY(f.popup->keepOneButton()->isEnabled());
        // The context menu offers it for a text copy.
        QMenu* menu = f.popup->createContextMenu(*f.t.repo->get(far), f.popup.get());
        QVERIFY(actionStartingWith(menu, QStringLiteral("Show similar copies")));
        delete menu;
    }

    void menuActionStartsTheView()
    {
        Fixture f;
        f.add(kBase.toLower());
        const qint64 ref = f.add(kBase);
        QMenu* menu = f.popup->createContextMenu(*f.t.repo->get(ref), f.popup.get());
        QAction* action = actionStartingWith(menu, QStringLiteral("Show similar copies"));
        QVERIFY(action);
        action->trigger();
        QVERIFY(f.popup->similarActive());
        QVERIFY(f.waitDone());
        QCOMPARE(f.shownIds().size(), 2);
        delete menu;
    }

    void passwordsAndPicturesAreNeverComparedOrOffered()
    {
        Fixture f;
        const qint64 ref = f.add(QStringLiteral("Password"));
        // A password copy's label looks like the text; it must not be matched, nor offered the view.
        const qint64 secret = f.t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:1"), QString(), QString()).id;
        const qint64 pic = f.t.repo->ingestImage(fakeImage(1)).id;
        QMenu* secretMenu = f.popup->createContextMenu(*f.t.repo->get(secret), f.popup.get());
        QVERIFY(!actionStartingWith(secretMenu, QStringLiteral("Show similar copies")));
        delete secretMenu;
        QMenu* picMenu = f.popup->createContextMenu(*f.t.repo->get(pic), f.popup.get());
        QVERIFY(!actionStartingWith(picMenu, QStringLiteral("Show similar copies")));
        delete picMenu;
        f.popup->showSimilar(secret);
        QVERIFY(!f.popup->similarActive());
        f.popup->showSimilar(pic);
        QVERIFY(!f.popup->similarActive());
        f.popup->showSimilar(ref);
        QVERIFY(f.waitDone());
        QCOMPARE(f.shownIds(), QList<qint64>{ref});
        QVERIFY(f.footer().contains(QStringLiteral("No similar copies")));
    }

    void keepThisOneAsksThenDeletesTheOthersAndSkipsProtectedOnes()
    {
        Fixture f;
        const qint64 pinned = f.add(kBase + QStringLiteral(" a"));
        f.t.repo->setPinned(pinned, true);
        const qint64 starred = f.add(kBase + QStringLiteral(" b"));
        f.t.repo->setFavorite(starred, true);
        const qint64 grouped = f.add(kBase + QStringLiteral(" c"));
        f.t.repo->setGroup(grouped, QStringLiteral("Work"));
        const qint64 noted = f.add(kBase + QStringLiteral(" d"));
        f.t.repo->setNote(noted, QStringLiteral("a note"));
        const qint64 plain1 = f.add(kBase.toUpper());
        const qint64 plain2 = f.add(QStringLiteral(" ") + kBase + QStringLiteral("\n"));
        const qint64 keep = f.add(kBase);
        f.popup->showSimilar(keep);
        QVERIFY(f.waitDone());
        QCOMPARE(f.shownIds().size(), 7);

        f.answer = false; // No: nothing is deleted
        f.popup->keepOnlyThis(keep);
        QCOMPARE(f.questions.size(), 1);
        QVERIFY(f.t.repo->get(plain1) && f.t.repo->get(plain2));

        f.answer = true;
        f.questions.clear();
        f.popup->keepOnlyThis(keep);
        QCOMPARE(f.questions.size(), 1);
        QVERIFY2(f.questions[0].startsWith(QStringLiteral("Delete 2 similar copies and keep this one?")),
                 qPrintable(f.questions[0]));
        QVERIFY2(f.questions[0].contains(QStringLiteral("4 pinned, starred, grouped or noted copies are not deleted")),
                 qPrintable(f.questions[0]));
        QVERIFY(!f.t.repo->get(plain1));
        QVERIFY(!f.t.repo->get(plain2));
        for (const qint64 id : {pinned, starred, grouped, noted, keep}) {
            QVERIFY(f.t.repo->get(id));
        }
        QVERIFY(f.footer().contains(QStringLiteral("Deleted 2 similar copies. 4 kept")));
        QVERIFY(f.popup->undoPending());

        // One Undo puts both back.
        QVERIFY(f.popup->undoPasteOnce());
        QVERIFY(!f.popup->similarActive());
        QCOMPARE(f.t.repo->count(), qint64(7));
        QVERIFY(f.t.repo->findByHash(f.t.repo->textHash(kBase.toUpper())).has_value());
    }

    void nothingToDeleteSaysSoWithoutAsking()
    {
        Fixture f;
        const qint64 pinned = f.add(kBase + QStringLiteral(" a"));
        f.t.repo->setPinned(pinned, true);
        const qint64 keep = f.add(kBase);
        f.popup->showSimilar(keep);
        QVERIFY(f.waitDone());
        f.popup->keepOnlyThis(keep);
        QVERIFY(f.questions.isEmpty());
        QCOMPARE(f.messages.size(), 1);
        QVERIFY(f.messages[0].contains(QStringLiteral("Nothing to delete")));
        QVERIFY(f.t.repo->get(pinned));
    }

    void theConfirmationIsSingularForOne()
    {
        Fixture f;
        f.add(kBase.toUpper());
        const qint64 keep = f.add(kBase);
        f.answer = false;
        f.popup->showSimilar(keep);
        QVERIFY(f.waitDone());
        f.popup->keepOnlyThis(keep);
        QCOMPARE(f.questions, QStringList{QStringLiteral("Delete 1 similar copy and keep this one?")});
    }

    void leavingTheViewBringsBackTheNormalList()
    {
        Fixture f;
        f.add(QStringLiteral("unrelated one"));
        const qint64 ref = f.add(kBase);
        f.popup->showSimilar(ref);
        QVERIFY(f.waitDone());
        QCOMPARE(f.shownIds().size(), 1);
        f.popup->leaveSimilar();
        QVERIFY(!f.popup->similarActive());
        QVERIFY(!f.popup->keepOneButton()->isVisible());
        QCOMPARE(f.shownIds().size(), 2);
        // Typing in the search box leaves it too.
        f.popup->showSimilar(ref);
        f.popup->searchBar()->setText(QStringLiteral("unrelated"));
        QVERIFY(!f.popup->similarActive());
    }

    void whenTheBudgetRunsOutItShowsWhatWasFoundWithANote()
    {
        Fixture f;
        // Long texts that are close to nothing: each comparison costs a little, 2,000 of them more than 1 ms.
        for (int i = 0; i < 2000; ++i) {
            f.add(QStringLiteral("filler %1 ").arg(i) + QString(3000, QChar('a' + i % 20)));
        }
        const qint64 ref = f.add(kBase);
        f.popup->setSimilarBudgetMs(1);
        f.popup->showSimilar(ref);
        QVERIFY(f.waitDone());
        QVERIFY(f.shownIds().contains(ref));
        QVERIFY2(f.footer().contains(QStringLiteral("Stopped after 1 ms")), qPrintable(f.footer()));
    }

    void aBigHistoryNeverStallsTheGuiThread()
    {
        Fixture f;
        // 5,000 long copies, half of them close to the reference (the costly kind of comparison), half far from it.
        const QString body = QString(similar::kMaxChars - 40, QLatin1Char('q'));
        for (int i = 0; i < 5000; ++i) {
            QString text = i % 2 == 0 ? body : QString(body.size(), QChar('a' + i % 20));
            text.replace(100 + i % 50, 1, QChar('A' + i % 26));
            text.replace(300 + i % 50, 1, QStringLiteral("%1").arg(i % 10));
            f.add(text + QString::number(i)); // each text is its own copy
        }
        const qint64 ref = f.add(body);
        f.popup->setSimilarBudgetMs(StallWatchdog::scaled(similar::kBudgetMs)); // slower builds get more time
        StallWatchdog wd;
        f.popup->showSimilar(ref);
        QVERIFY(f.waitDone());
        QVERIFY_NO_STALL(wd, "similar copies over 5,000 long copies");
        QVERIFY(f.shownIds().contains(ref));
        // How much is found depends on the machine (a sanitizer build is several times slower): when the budget ended
        // the scan the note says so, otherwise the close copies must all be there.
        const bool cutOff = f.footer().contains(QStringLiteral("Stopped after"));
        QVERIFY(cutOff || f.shownIds().size() >= 2);
        // Either the whole history was read or the budget ended the scan with a note: never an endless one.
        QVERIFY(f.footer().contains(QStringLiteral("similar cop")));
    }

    void nothingButCountsIsLogged()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*=true"));
        g_log.clear();
        const QtMessageHandler previous = qInstallMessageHandler(captureLog);
        {
            Fixture f;
            const QString marker = QStringLiteral("zqxj-marker-text-for-log-check");
            f.add(marker.toUpper());
            f.add(marker + QStringLiteral(" "));
            const qint64 ref = f.add(marker);
            f.popup->showSimilar(ref);
            QVERIFY(f.waitDone());
            f.popup->keepOnlyThis(ref);
            f.popup->undoPasteOnce();
        }
        qInstallMessageHandler(previous);
        QLoggingCategory::setFilterRules(QString());
        for (const QString& line : std::as_const(g_log)) {
            QVERIFY2(!line.contains(QStringLiteral("zqxj"), Qt::CaseInsensitive), "a copy's text was logged");
        }
        QVERIFY(std::any_of(g_log.begin(), g_log.end(), [](const QString& l) { return l.contains(QStringLiteral("similar")); }));
    }
};

QTEST_MAIN(TestSimilar)
#include "tst_similar.moc"
