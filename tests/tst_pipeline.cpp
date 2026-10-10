#include "CapturePipeline.h"
#include "TestDb.h"

#include <QSignalSpy>
#include <QTest>

using clipitto::CapturePipeline;
using clipitto::ClipFilter;

class TestPipeline : public QObject {
    Q_OBJECT

private slots:
    void storesAcceptedText()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 1024);
        QSignalSpy stored(&p, &CapturePipeline::stored);

        p.process(QStringLiteral("hello"), {QStringLiteral("text/plain")});
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.at(0).at(1).toBool(), true);
        QCOMPARE(t.repo->count(), 1);

        p.process(QStringLiteral("hello"), {});
        QCOMPARE(stored.size(), 2);
        QCOMPARE(stored.at(1).at(1).toBool(), false); // dedupe bump
        QCOMPARE(t.repo->count(), 1);
    }

    void rejectsFilteredText()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 8);
        QSignalSpy rejected(&p, &CapturePipeline::rejected);

        p.process(QStringLiteral("   "), {});
        p.process(QStringLiteral("way too long"), {});
        p.process(QStringLiteral("pw"), {QStringLiteral("x-kde-passwordManagerHint")});
        QCOMPARE(rejected.size(), 3);
        QCOMPARE(t.repo->count(), 0);
    }

    void prunesToLimitOnInsert()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 10, 1024);
        const auto fav = t.repo->ingest(QStringLiteral("keep me")).id;
        t.repo->setFavorite(fav, true);

        for (int i = 0; i < 25; ++i) {
            p.process(QStringLiteral("clip %1").arg(i), {});
        }
        QCOMPARE(t.repo->countUnsaved(), 10);
        QVERIFY(t.repo->get(fav));
        QCOMPARE(t.repo->recent(1, true).front().content, QStringLiteral("clip 24"));
    }

    void setLimitsAppliesToNextInsert()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 1024);
        for (int i = 0; i < 20; ++i) {
            p.process(QStringLiteral("clip %1").arg(i), {});
        }
        p.setLimits(5, 4);
        p.process(QStringLiteral("toolong"), {}); // rejected by the new byte limit, so no prune yet
        QCOMPARE(t.repo->countUnsaved(), 20);
        p.process(QStringLiteral("new"), {});
        QCOMPARE(t.repo->countUnsaved(), 5);
    }

    void ignoresOwnWrites()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 1024);
        p.filter().noteOwnWrite(QStringLiteral("from popup"));
        p.process(QStringLiteral("from popup"), {});
        QCOMPARE(t.repo->count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestPipeline)
#include "tst_pipeline.moc"
