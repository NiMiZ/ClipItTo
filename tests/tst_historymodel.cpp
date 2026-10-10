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

    void previewLinesKeepsFirstNonBlankLines()
    {
        using clipitto::ui::previewLines;
        QCOMPARE(previewLines(QStringLiteral("a\n\n  b  \nc"), 2), (QStringList{QStringLiteral("a"), QStringLiteral("b\u2026")}));
        QCOMPARE(previewLines(QStringLiteral("a\nb"), 5), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(previewLines(QStringLiteral("  \n "), 3).isEmpty());
    }

    void titleIsNoteWhenSetElseContent()
    {
        HistoryModel m;
        m.setContent({{QString(), {entry(1, QStringLiteral("kubectl get pods -A"), QStringLiteral("all pods")),
                                   entry(2, QStringLiteral("plain\ntext"))}}});
        QCOMPARE(m.index(0, 0).data(HistoryModel::TitleRole).toString(), QStringLiteral("all pods"));
        QCOMPARE(m.index(0, 0).data(HistoryModel::PreviewRole).toString(), QStringLiteral("kubectl get pods -A"));
        QVERIFY(m.index(0, 0).data(HistoryModel::HasNoteRole).toBool());
        QCOMPARE(m.index(1, 0).data(HistoryModel::TitleRole).toString(), QStringLiteral("plain text"));
        QVERIFY(m.index(1, 0).data(HistoryModel::PreviewRole).toString().isEmpty());
    }

    void severalPreviewLines()
    {
        HistoryModel m;
        m.setPreviewLines(2);
        m.setContent({{QString(), {entry(1, QStringLiteral("first\nsecond\nthird")),
                                   entry(2, QStringLiteral("body\nmore"), QStringLiteral("note"))}}});
        QCOMPARE(m.index(0, 0).data(HistoryModel::TitleRole).toString(), QStringLiteral("first"));
        QCOMPARE(m.index(0, 0).data(HistoryModel::PreviewRole).toString(), QStringLiteral("second\u2026"));
        QCOMPARE(m.index(1, 0).data(HistoryModel::TitleRole).toString(), QStringLiteral("note"));
        QCOMPARE(m.index(1, 0).data(HistoryModel::PreviewRole).toString(), QStringLiteral("body\nmore"));
    }

    void headersAreInert()
    {
        HistoryModel m;
        m.setContent({{QStringLiteral("Saved"), {entry(10, QStringLiteral("a"))}},
                      {QStringLiteral("Recent"), {entry(20, QStringLiteral("b"))}}});
        QCOMPARE(m.rowCount(), 4);
        QCOMPARE(m.flags(m.index(0, 0)), Qt::NoItemFlags);
        QVERIFY(m.flags(m.index(1, 0)) & Qt::ItemIsSelectable);
        QVERIFY(m.index(2, 0).data(HistoryModel::IsHeaderRole).toBool());
        QVERIFY(!m.entryAt(m.index(0, 0)));
        QCOMPARE(m.entryAt(m.index(3, 0))->id, 20);
        QCOMPARE(m.firstEntry().row(), 1);
        QCOMPARE(m.findEntry(20).row(), 3);
        QVERIFY(!m.findEntry(99).isValid());
    }

    void groupTreeStructure()
    {
        Entry a = entry(1, QStringLiteral("a"));
        a.group = QStringLiteral("Work/Servers");
        Entry b = entry(2, QStringLiteral("b"));
        b.group = QStringLiteral("Work");
        HistoryModel m;
        m.setContent({{QString(), {entry(3, QStringLiteral("c"))}}},
                     {{QStringLiteral("Home"), 0}, {QStringLiteral("Work"), 1}, {QStringLiteral("Work/Servers"), 1}},
                     {a, b});

        const QModelIndex root = m.groupsRoot();
        QCOMPARE(root.row(), 0);
        QCOMPARE(m.kindAt(root), HistoryModel::Kind::GroupsRoot);
        QVERIFY(m.isExpandable(root));
        QCOMPARE(root.data(HistoryModel::CountRole).toInt(), 2);
        QCOMPARE(m.firstEntry().row(), 1); // the first copy outside the tree

        const QModelIndex home = m.findGroup(QStringLiteral("Home"));
        QVERIFY(home.isValid());
        QVERIFY(!m.isExpandable(home)); // empty group
        QCOMPARE(m.pathAt(home), QStringLiteral("Home"));

        const QModelIndex work = m.findGroup(QStringLiteral("Work"));
        QCOMPARE(m.rowCount(work), 2);
        QCOMPARE(m.findGroup(QStringLiteral("Work/Servers")).parent(), work);
        const QModelIndex inServers = m.findEntry(1);
        QCOMPARE(inServers.parent(), m.findGroup(QStringLiteral("Work/Servers")));
        QCOMPARE(m.pathAt(inServers), QStringLiteral("Work/Servers"));
        QVERIFY(!m.findGroup(QStringLiteral("Work/Nope")).isValid());
        QCOMPARE(m.parent(root), QModelIndex());
    }

    void emptyModel()
    {
        HistoryModel m;
        QCOMPARE(m.rowCount(), 0);
        QVERIFY(!m.firstEntry().isValid());
        QVERIFY(!m.groupsRoot().isValid());
    }
};

QTEST_GUILESS_MAIN(TestHistoryModel)
#include "tst_historymodel.moc"
