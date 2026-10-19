#include "CapturePipeline.h"
#include "TestDb.h"

#include <QSignalSpy>
#include <QTest>

using clipitto::CapturePipeline;
using clipitto::ClipFilter;


namespace {

// A stored picture as the capture side would hand it over (the bytes needn't decode here).
clipitto::ImageClip fakeImage(int n, int w = 800, int h = 600)
{
    clipitto::ImageClip clip;
    clip.png = QByteArray("PNG-") + QByteArray::number(n) + QByteArray(64, 'x');
    clip.thumbnail = QByteArray("THUMB-") + QByteArray::number(n);
    clip.width = w;
    clip.height = h;
    clip.pixelHash = QStringLiteral("hash%1").arg(n);
    return clip;
}

} // namespace

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

    void storesPicturesWithinTheirLimits()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 1024);
        p.setImageLimits(2, 1000);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy rejected(&p, &CapturePipeline::rejected);

        for (int i = 0; i < 3; ++i) {
            p.processImage(fakeImage(i), {QStringLiteral("image/png")});
        }
        QCOMPARE(stored.size(), 3);
        QCOMPARE(t.repo->countImages(), 2); // pruned to the image limit

        clipitto::ImageClip big = fakeImage(9);
        big.png = QByteArray(1001, 'x');
        p.processImage(big, {});
        p.processImage(fakeImage(8), {QStringLiteral("image/png"), QStringLiteral("x-kde-passwordManagerHint")});
        QCOMPARE(rejected.size(), 2);
        QCOMPARE(rejected.at(0).at(0).value<ClipFilter::Verdict>(), ClipFilter::Verdict::TooLarge);
        QCOMPARE(rejected.at(1).at(0).value<ClipFilter::Verdict>(), ClipFilter::Verdict::Sensitive);

        // Our own pasted picture coming back is not a new copy.
        p.filter().noteOwnImage(fakeImage(7).pixelHash);
        p.processImage(fakeImage(7), {});
        QCOMPARE(rejected.size(), 3);
        QCOMPARE(rejected.at(2).at(0).value<ClipFilter::Verdict>(), ClipFilter::Verdict::OwnWrite);

        // 0 = pictures off: nothing is stored, nothing reported.
        p.setImageLimits(0, 1000);
        p.processImage(fakeImage(20), {});
        QCOMPARE(t.repo->countImages(), 2);
        QCOMPARE(rejected.size(), 3);
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

    void pausedClipsWaitAndAreStoredOnResume()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 1024);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        p.setPaused(true);
        p.process(QStringLiteral("first"), {});
        p.process(QStringLiteral("second"), {});
        p.process(QStringLiteral("first"), {}); // again: one entry, moved to the end
        p.process(QStringLiteral("   "), {}); // still filtered while paused
        QCOMPARE(t.repo->count(), 0);
        QCOMPARE(stored.size(), 0);

        p.setPaused(false);
        QCOMPARE(t.repo->count(), 2);
        QCOMPARE(stored.size(), 2);
        QCOMPARE(t.repo->recent(1, false).front().content, QStringLiteral("first"));
    }

    void pausedBufferIsBounded()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 10, 1024);
        p.setPaused(true);
        for (int i = 0; i < 50; ++i) {
            p.process(QStringLiteral("clip %1").arg(i), {});
        }
        p.setPaused(false);
        QCOMPARE(t.repo->count(), 10);
        QCOMPARE(t.repo->recent(1, false).front().content, QStringLiteral("clip 49"));
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
