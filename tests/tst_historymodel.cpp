#include "HistoryModel.h"

#include <QTest>

using clipitto::Entry;
using clipitto::ui::HistoryModel;

namespace {

Entry entry(qint64 id, const QString& content, const QString& note = {})
{
    Entry e;
    e.id = id;
    e.content = content;
    e.note = note;
    return e;
}

} // namespace

class TestHistoryModel : public QObject {
    Q_OBJECT

private slots:
    void previewCollapsesWhitespaceAndCaps()
    {
        QCOMPARE(clipitto::ui::previewText(QStringLiteral("  line one\n\tline two  ")),
                 QStringLiteral("line one line two"));
        const QString preview = clipitto::ui::previewText(QString(500, QLatin1Char('x')), 10);
        QCOMPARE(preview, QString(10, QLatin1Char('x')) + QChar(0x2026));
    }

    void relativeTimeBuckets()
    {
        const QDateTime now(QDate(2026, 10, 10), QTime(12, 0), Qt::UTC);
        QCOMPARE(clipitto::ui::relativeTime(now.addSecs(-5), now), QStringLiteral("now"));
        QCOMPARE(clipitto::ui::relativeTime(now.addSecs(-5 * 60), now), QStringLiteral("5m"));
        QCOMPARE(clipitto::ui::relativeTime(now.addSecs(-3 * 3600), now), QStringLiteral("3h"));
        QCOMPARE(clipitto::ui::relativeTime(now.addDays(-2), now), QStringLiteral("2d"));
        QVERIFY(!clipitto::ui::relativeTime(now.addDays(-30), now).isEmpty());
        QVERIFY(clipitto::ui::relativeTime(QDateTime(), now).isEmpty());
    }

    void titleIsNoteWhenSetElseContent()
    {
        HistoryModel m;
        m.setRows({{{}, entry(1, QStringLiteral("kubectl get pods -A"), QStringLiteral("all pods"))},
                   {{}, entry(2, QStringLiteral("plain\ntext"))}});
        QCOMPARE(m.index(0).data(HistoryModel::TitleRole).toString(), QStringLiteral("all pods"));
        QCOMPARE(m.index(0).data(HistoryModel::PreviewRole).toString(), QStringLiteral("kubectl get pods -A"));
        QCOMPARE(m.index(1).data(HistoryModel::TitleRole).toString(), QStringLiteral("plain text"));
        QVERIFY(m.index(1).data(HistoryModel::PreviewRole).toString().isEmpty());
    }

    void headersAreInertAndSkipped()
    {
        HistoryModel m;
        m.setRows({{QStringLiteral("Saved"), {}},
                   {{}, entry(10, QStringLiteral("a"))},
                   {QStringLiteral("Recent"), {}},
                   {{}, entry(20, QStringLiteral("b"))}});
        QCOMPARE(m.flags(m.index(0)), Qt::NoItemFlags);
        QVERIFY(m.flags(m.index(1)) & Qt::ItemIsSelectable);
        QVERIFY(m.index(2).data(HistoryModel::IsHeaderRole).toBool());
        QVERIFY(!m.entryAt(0));
        QCOMPARE(m.entryAt(3)->id, 20);

        QCOMPARE(m.firstEntryRow(), 1);
        QCOMPARE(m.nextEntryRow(1, +1), 3);
        QCOMPARE(m.nextEntryRow(3, -1), 1);
        QCOMPARE(m.nextEntryRow(1, -1), -1);
        QCOMPARE(m.rowForId(20), 3);
        QCOMPARE(m.rowForId(99), -1);
    }

    void emptyModel()
    {
        HistoryModel m;
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.firstEntryRow(), -1);
    }
};

QTEST_GUILESS_MAIN(TestHistoryModel)
#include "tst_historymodel.moc"
