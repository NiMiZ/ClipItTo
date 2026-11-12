// Auto-forget (roadmap 18): the rule text, matching, and the Pruner against a real (in-memory or file) history.
// No clipboard contents are printed anywhere; texts here are synthetic.
#include "AutoForget.h"
#include "TestDb.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using clipitto::AutoForgetRule;
using clipitto::autoforget::Pruner;
namespace af = clipitto::autoforget;
using clipitto::db::Repository;

namespace {

AutoForgetRule age(int days, AutoForgetRule::Scope scope = AutoForgetRule::Scope::Any)
{
    AutoForgetRule r;
    r.type = AutoForgetRule::Type::Age;
    r.amount = days;
    r.scope = scope;
    return r;
}

AutoForgetRule contains(const QString& text, int minutes)
{
    AutoForgetRule r;
    r.type = AutoForgetRule::Type::Contains;
    r.amount = minutes;
    r.text = text;
    return r;
}

AutoForgetRule secret(AutoForgetRule::SecretKind kind, int minutes)
{
    AutoForgetRule r;
    r.type = AutoForgetRule::Type::Secret;
    r.amount = minutes;
    r.secret = kind;
    return r;
}

// Fixed, made-up secrets of the shapes the scanner knows (none is a real credential).
const QString kAwsKey = QStringLiteral("AKIAABCDEFGHIJKLMNOP");
const QString kGithubToken = QStringLiteral("ghp_") + QString(36, QLatin1Char('a'));
const QString kCard = QStringLiteral("4111 1111 1111 1111");

const QDateTime kBase(QDate(2026, 3, 1), QTime(8, 0), Qt::UTC);

clipitto::ImageClip picture(int n)
{
    clipitto::ImageClip clip;
    clip.png = QByteArray("PNG-") + QByteArray::number(n) + QByteArray(64, 'x');
    clip.thumbnail = QByteArray("THUMB-") + QByteArray::number(n);
    clip.width = 10;
    clip.height = 10;
    clip.pixelHash = QStringLiteral("afhash%1").arg(n);
    return clip;
}

// A history whose copies carry chosen times: set t.clock, then ingest.
struct Rig {
    TestDb t;
    QDateTime now = kBase;
    Pruner pruner{*t.repo, [this] { return now; }};

    qint64 ingestAt(const QDateTime& when, const QString& text)
    {
        t.clock = when;
        return t.repo->ingest(text).id;
    }
    bool has(qint64 id) { return t.repo->get(id).has_value(); }
};

} // namespace

class TestAutoForget : public QObject {
    Q_OBJECT

private slots:
    void ruleTextRoundTrips()
    {
        const QList<AutoForgetRule> rules = {
            age(30),
            age(1, AutoForgetRule::Scope::Picture),
            age(7, AutoForgetRule::Scope::Text),
            contains(QStringLiteral("Bearer "), 5),
            contains(QStringLiteral("a b"), 10080),
            secret(AutoForgetRule::SecretKind::Any, 3),
            secret(AutoForgetRule::SecretKind::Token, 3),
            secret(AutoForgetRule::SecretKind::Key, 3),
            secret(AutoForgetRule::SecretKind::Card, 3),
        };
        for (const AutoForgetRule& rule : rules) {
            QVERIFY(af::validate(rule).isEmpty());
            AutoForgetRule back;
            QVERIFY2(af::parse(af::format(rule), back), qPrintable(af::format(rule)));
            QVERIFY(back == rule);
        }
        QCOMPARE(af::format(age(30)), QStringLiteral("age 30d"));
        QCOMPARE(af::format(age(1, AutoForgetRule::Scope::Picture)), QStringLiteral("age 1d picture"));
        QCOMPARE(af::format(contains(QStringLiteral("Bearer "), 5)), QStringLiteral("match 5m contains \"Bearer \""));
        QCOMPARE(af::format(secret(AutoForgetRule::SecretKind::Card, 9)), QStringLiteral("match 9m card"));
    }

    void badRuleTextIsRefused_data()
    {
        QTest::addColumn<QString>("text");
        for (const char* text : {"", "age", "age 0d", "age -1d", "age 3651d", "age 30", "age 30d video", "age 30d text x",
                                 "age xd", "match", "match 5m", "match 0m token", "match 10081m token",
                                 "match 5m contains", "match 5m contains Bearer", "match 5m contains \"ab\"",
                                 "match 5m contains \"\"", "match 5m contains \"a\"b c\"", "match 5m nonsense",
                                 "match 5 token", "after 5m token", "match 5m token extra"}) {
            QTest::newRow(text) << QString::fromLatin1(text);
        }
        QTest::newRow("over 100 characters") << QStringLiteral("match 5m contains \"") + QString(101, QLatin1Char('x'))
                + QStringLiteral("\"");
    }

    void badRuleTextIsRefused()
    {
        QFETCH(QString, text);
        AutoForgetRule untouched = age(77);
        AutoForgetRule out = untouched;
        QVERIFY(!af::parse(text, out));
        QVERIFY(out == untouched); // left alone
    }

    void validateBounds()
    {
        QVERIFY(!af::validate(age(0)).isEmpty());
        QVERIFY(!af::validate(age(af::kMaxDays + 1)).isEmpty());
        QVERIFY(af::validate(age(af::kMaxDays)).isEmpty());
        QVERIFY(!af::validate(secret(AutoForgetRule::SecretKind::Any, 0)).isEmpty());
        QVERIFY(!af::validate(secret(AutoForgetRule::SecretKind::Any, af::kMaxMinutes + 1)).isEmpty());
        QVERIFY(!af::validate(contains(QStringLiteral("ab"), 5)).isEmpty()); // too short to be safe
        QVERIFY(!af::validate(contains(QString(af::kMaxTextLength + 1, QLatin1Char('x')), 5)).isEmpty());
        QVERIFY(!af::validate(contains(QStringLiteral("a\"b"), 5)).isEmpty());
        QVERIFY(!af::validate(contains(QStringLiteral("a\nb"), 5)).isEmpty());
        QVERIFY(af::validate(contains(QStringLiteral("abc"), 5)).isEmpty());
    }

    void matching()
    {
        QVERIFY(af::matchesText(contains(QStringLiteral("Bearer "), 5), QStringLiteral("Authorization: bearer abc")));
        QVERIFY(!af::matchesText(contains(QStringLiteral("Bearer "), 5), QStringLiteral("Authorization: Basic abc")));
        QVERIFY(!af::matchesText(age(5), QStringLiteral("anything"))); // time only

        using K = AutoForgetRule::SecretKind;
        const struct {
            const QString& text;
            K kind;
        } found[] = {{kAwsKey, K::Key}, {kGithubToken, K::Token}, {kCard, K::Card}};
        for (const auto& f : found) {
            QVERIFY(af::matchesText(secret(K::Any, 1), f.text));
            for (const K k : {K::Token, K::Key, K::Card}) {
                QCOMPARE(af::matchesText(secret(k, 1), f.text), k == f.kind);
            }
        }
        QVERIFY(!af::matchesText(secret(K::Any, 1), QStringLiteral("just some words")));
        QVERIFY(af::matchesText(secret(K::Key, 1), QStringLiteral("-----BEGIN RSA PRIVATE KEY-----")));
    }

    void noRulesNothingHappens()
    {
        Rig r;
        const qint64 id = r.ingestAt(kBase.addYears(-5), QStringLiteral("ancient copy"));
        r.now = kBase;
        QCOMPARE(r.pruner.run(), qint64(0));
        QVERIFY(r.has(id));
    }

    void ageRemovesOldCopiesOnly()
    {
        Rig r;
        const qint64 old = r.ingestAt(kBase.addDays(-40), QStringLiteral("old"));
        const qint64 mid = r.ingestAt(kBase.addDays(-20), QStringLiteral("middle"));
        const qint64 fresh = r.ingestAt(kBase.addDays(-1), QStringLiteral("fresh"));
        r.pruner.setRules({age(30)});
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(old));
        QVERIFY(r.has(mid) && r.has(fresh));
        QCOMPARE(r.pruner.run(), qint64(0)); // nothing more is due

        // Time moves on: the middle one becomes old. A copy used again counts from then (last_used).
        r.t.clock = kBase.addDays(9);
        r.t.repo->ingest(QStringLiteral("middle")); // copied again
        r.now = kBase.addDays(15);
        QCOMPARE(r.pruner.run(), qint64(0));
        r.now = kBase.addDays(45);
        QCOMPARE(r.pruner.run(), qint64(2)); // fresh (45-1 > 30) and middle (45-9 > 30)
        QVERIFY(!r.has(mid) && !r.has(fresh));
    }

    void ageByKind()
    {
        Rig r;
        const QDateTime when = kBase.addDays(-3);
        const qint64 text = r.ingestAt(when, QStringLiteral("text copy"));
        r.t.clock = when;
        const qint64 pic = r.t.repo->ingestImage(picture(1)).id;
        r.pruner.setRules({age(1, AutoForgetRule::Scope::Picture)});
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(r.has(text));
        QVERIFY(!r.has(pic));
        QVERIFY(r.t.repo->imageData(pic).isEmpty()); // the picture went with it

        const qint64 pic2 = [&] { r.t.clock = when; return r.t.repo->ingestImage(picture(2)).id; }();
        r.pruner.setRules({age(1, AutoForgetRule::Scope::Text)});
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(text));
        QVERIFY(r.has(pic2));

        r.pruner.setRules({age(1)});
        QCOMPARE(r.pruner.run(), qint64(1)); // any kind
        QVERIFY(!r.has(pic2));
    }

    void savedAndPasswordCopiesAreNeverRemoved()
    {
        Rig r;
        const QDateTime old = kBase.addDays(-400);
        const qint64 pinned = r.ingestAt(old, QStringLiteral("pinned ") + kAwsKey + QStringLiteral(" Bearer "));
        const qint64 starred = r.ingestAt(old, QStringLiteral("starred ") + kAwsKey + QStringLiteral(" Bearer "));
        const qint64 grouped = r.ingestAt(old, QStringLiteral("grouped ") + kAwsKey + QStringLiteral(" Bearer "));
        const qint64 plain = r.ingestAt(old, QStringLiteral("plain ") + kAwsKey + QStringLiteral(" Bearer "));
        QCOMPARE(r.t.repo->setPinned(pinned, true), Repository::Change::Done);
        QCOMPARE(r.t.repo->setFavorite(starred, true), Repository::Change::Done);
        QCOMPARE(r.t.repo->setGroup(grouped, QStringLiteral("Work")), Repository::Change::Done);
        r.t.clock = old;
        const qint64 pw = r.t.repo->ingestSecret("SEALED", QStringLiteral("secret:aa"), QString(), QString()).id;
        r.t.clock = old;
        const qint64 pwGrouped
            = r.t.repo->ingestSecret("SEALED2", QStringLiteral("secret:bb"), QString(), QStringLiteral("Passwords")).id;

        r.pruner.setRules({age(1), contains(QStringLiteral("Bearer "), 1), secret(AutoForgetRule::SecretKind::Any, 1)});
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(plain));
        QVERIFY(r.has(pinned) && r.has(starred) && r.has(grouped) && r.has(pw) && r.has(pwGrouped));
        QCOMPARE(r.t.repo->sealedSecret(pw), QByteArray("SEALED")); // the sealed copy is untouched
        QCOMPARE(r.t.repo->countSecrets(), qint64(2));
        // And again, much later.
        r.now = kBase.addYears(10);
        r.pruner.run();
        QVERIFY(r.has(pinned) && r.has(starred) && r.has(grouped) && r.has(pw) && r.has(pwGrouped));
    }

    void aCopyFiledAfterBeingSeenIsKept()
    {
        // The rule looked at the copy while it was unsaved; the user pins it before the time is up.
        Rig r;
        const qint64 id = r.ingestAt(kBase.addSecs(-1 * 60), QStringLiteral("token ") + kAwsKey);
        r.pruner.setRules({secret(AutoForgetRule::SecretKind::Key, 5), contains(QStringLiteral("zzz-none"), 1)});
        r.now = kBase.addSecs(2 * 60); // old enough to be looked at (the 1 minute rule), not yet due (5)
        QCOMPARE(r.pruner.run(), qint64(0));
        QCOMPARE(r.t.repo->setFavorite(id, true), Repository::Change::Done);
        r.now = kBase.addSecs(10 * 60);
        QCOMPARE(r.pruner.run(), qint64(0));
        QVERIFY(r.has(id));
    }

    void patternsWaitForTheirMinutes()
    {
        Rig r;
        const qint64 bearer = r.ingestAt(kBase, QStringLiteral("curl -H 'Authorization: Bearer abc123'"));
        const qint64 key = r.ingestAt(kBase, QStringLiteral("key=") + kAwsKey);
        const qint64 token = r.ingestAt(kBase, kGithubToken);
        const qint64 card = r.ingestAt(kBase, kCard);
        const qint64 other = r.ingestAt(kBase, QStringLiteral("a perfectly ordinary note"));
        r.pruner.setRules({contains(QStringLiteral("bearer "), 5), secret(AutoForgetRule::SecretKind::Token, 10),
                           secret(AutoForgetRule::SecretKind::Key, 20), secret(AutoForgetRule::SecretKind::Card, 30)});

        r.now = kBase.addSecs(4 * 60);
        QCOMPARE(r.pruner.run(), qint64(0));
        r.now = kBase.addSecs(6 * 60);
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(bearer));
        r.now = kBase.addSecs(11 * 60);
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(token));
        r.now = kBase.addSecs(21 * 60);
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(key));
        r.now = kBase.addSecs(31 * 60);
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(card));
        r.now = kBase.addDays(30);
        QCOMPARE(r.pruner.run(), qint64(0));
        QVERIFY(r.has(other)); // no rule says anything about it
    }

    void aCopiedAgainCopyStartsItsMinutesAgain()
    {
        Rig r;
        const qint64 id = r.ingestAt(kBase, QStringLiteral("x Bearer y"));
        r.pruner.setRules({contains(QStringLiteral("Bearer"), 5)});
        r.now = kBase.addSecs(3 * 60);
        QCOMPARE(r.pruner.run(), qint64(0));
        r.t.clock = r.now;
        QCOMPARE(r.t.repo->ingest(QStringLiteral("x Bearer y")).id, id); // copied again
        r.now = kBase.addSecs(7 * 60); // 4 minutes after the second copy
        QCOMPARE(r.pruner.run(), qint64(0));
        r.now = kBase.addSecs(9 * 60);
        QCOMPARE(r.pruner.run(), qint64(1));
    }

    void changingTheRulesLooksAgain()
    {
        Rig r;
        const qint64 id = r.ingestAt(kBase, QStringLiteral("hello Bearer world"));
        const qint64 other = r.ingestAt(kBase, QStringLiteral("hello world"));
        r.now = kBase.addSecs(3600);
        r.pruner.setRules({contains(QStringLiteral("nothing like it"), 1)});
        QCOMPARE(r.pruner.run(), qint64(0)); // looked at and remembered as not matching
        r.pruner.setRules({contains(QStringLiteral("Bearer"), 1)});
        QCOMPARE(r.pruner.run(), qint64(1));
        QVERIFY(!r.has(id));
        QVERIFY(r.has(other));
    }

    void anEditedCopyIsLookedAtAgain()
    {
        Rig r;
        const qint64 id = r.ingestAt(kBase, QStringLiteral("harmless text"));
        r.pruner.setRules({contains(QStringLiteral("Bearer"), 1)});
        r.now = kBase.addSecs(3600);
        QCOMPARE(r.pruner.run(), qint64(0));
        QCOMPARE(r.t.repo->setContent(id, QStringLiteral("now with Bearer in it"), 1 << 20), Repository::ContentChange::Done);
        QCOMPARE(r.pruner.run(), qint64(1));
    }

    void aRunRemovesABoundedNumberAndSaysThereIsMore()
    {
        Rig r;
        const int n = 2 * Pruner::kDeleteLimit + 50;
        {
            clipitto::db::Transaction tx(r.t.db);
            for (int i = 0; i < n; ++i) {
                r.t.clock = kBase.addMSecs(i);
                r.t.repo->ingest(QStringLiteral("line %1 Bearer").arg(i));
            }
            tx.commit();
        }
        r.pruner.setRules({contains(QStringLiteral("Bearer"), 1), age(1)});
        r.now = kBase.addDays(5);
        QCOMPARE(r.pruner.run(), qint64(Pruner::kDeleteLimit));
        QVERIFY(r.pruner.hasMore());
        QCOMPARE(r.pruner.run(), qint64(Pruner::kDeleteLimit));
        QVERIFY(r.pruner.hasMore());
        QCOMPARE(r.pruner.run(), qint64(50));
        QVERIFY(!r.pruner.hasMore());
        QCOMPARE(r.t.repo->count(), qint64(0));
        QCOMPARE(r.pruner.run(), qint64(0));
    }

    void aRunReadsABoundedNumberOfCopies()
    {
        Rig r;
        const int n = Pruner::kScanRows + 100;
        {
            clipitto::db::Transaction tx(r.t.db);
            for (int i = 0; i < n; ++i) {
                r.t.clock = kBase.addMSecs(i);
                r.t.repo->ingest(QStringLiteral("line %1").arg(i));
            }
            r.t.clock = kBase.addMSecs(n); // the newest, so the last one the scan reaches
            r.t.repo->ingest(QStringLiteral("the one with Bearer"));
            tx.commit();
        }
        r.pruner.setRules({contains(QStringLiteral("Bearer"), 1)});
        r.now = kBase.addDays(1);
        QCOMPARE(r.pruner.run(), qint64(0)); // the first run read only kScanRows copies
        QVERIFY(r.pruner.hasMore());
        QCOMPARE(r.pruner.run(), qint64(1)); // the next reached it (what was read before is remembered)
        QVERIFY(!r.pruner.hasMore());
        QCOMPARE(r.t.repo->count(), qint64(n));
    }

    void removedTextIsNotLeftInTheFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("history.db"));
        const auto fileHolds = [&](const QByteArray& needle) {
            for (const QString& name : {path, path + QStringLiteral("-wal")}) {
                QFile f(name);
                if (f.open(QIODevice::ReadOnly) && f.readAll().contains(needle)) {
                    return true;
                }
            }
            return false;
        };
        QDateTime now = kBase;
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            QDateTime clock = kBase.addDays(-10);
            Repository repo(db, [&clock] { clock = clock.addMSecs(1); return clock; });
            Pruner pruner(repo, [&now] { return now; });
            repo.ingest(QStringLiteral("zqxw-by-age-mkr"));
            clock = kBase.addSecs(-30 * 60);
            repo.ingest(QStringLiteral("zqxw-by-pattern-Bearer-mkr"));
            clock = kBase.addSecs(-30 * 60);
            repo.ingest(QStringLiteral("zqxw-kept-mkr"));
            QVERIFY(fileHolds("zqxw-by-age-mkr"));
            QVERIFY(fileHolds("zqxw-by-pattern-Bearer-mkr"));

            pruner.setRules({age(5), contains(QStringLiteral("Bearer"), 10)});
            QCOMPARE(pruner.run(), qint64(2));
            QVERIFY(!fileHolds("zqxw-by-age-mkr"));
            QVERIFY(!fileHolds("zqxw-by-pattern-Bearer-mkr"));
            QVERIFY(fileHolds("zqxw-kept-mkr"));
            QCOMPARE(repo.search(QStringLiteral("zqxw")).size(), size_t(1)); // gone from the search index too
        }
    }
};

QTEST_GUILESS_MAIN(TestAutoForget)
#include "tst_autoforget.moc"
