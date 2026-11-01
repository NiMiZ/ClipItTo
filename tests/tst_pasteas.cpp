#include "PasteAs.h"

#include <QSet>
#include <QTest>

using namespace clipitto::pasteas;

class TestPasteAs : public QObject {
    Q_OBJECT

private:
    static QStringList ids(const QList<Choice>& c)
    {
        QStringList out;
        for (const Choice& x : c) {
            out << (x.shown ? x.id : QLatin1Char('-') + x.id);
        }
        return out;
    }

private slots:
    void idsAreUniqueAndStable()
    {
        for (const QList<Item>& items : {textItems(), pictureItems()}) {
            QSet<QString> seen;
            for (const Item& i : items) {
                QVERIFY(!i.id.isEmpty());
                QVERIFY(!seen.contains(i.id));
                seen.insert(i.id);
            }
        }
        const QList<Item> text = textItems();
        QCOMPARE(text.first().id, QStringLiteral("upper"));
        QCOMPARE(text.at(text.size() - 2).id, QStringLiteral("layout"));
        QCOMPARE(text.last().id, QStringLiteral("translate"));
        QCOMPARE(text.size(), 14);
        QCOMPARE(pictureItems().size(), 4);
    }

    void emptySettingIsTheDefault()
    {
        const QList<Item> items = textItems();
        QCOMPARE(resolve(QString(), items).size(), items.size());
        QVERIFY(resolve(QStringLiteral("  "), items).first() == (Choice{QStringLiteral("upper"), true}));
        QCOMPARE(serialize(resolve(QString(), items), items), QString());
        // Only unknown ids: also the default.
        QCOMPARE(serialize(resolve(QStringLiteral("nope,zzz"), items), items), QString());
    }

    void orderHidingAndNewItems()
    {
        const QList<Item> items = pictureItems();
        const QList<Choice> c = resolve(QStringLiteral("pic_paste, -pic_text,bogus,pic_paste"), items);
        QCOMPARE(ids(c), (QStringList{"pic_paste", "-pic_text", "pic_changed", "pic_translate"}));
        QCOMPARE(shownIds(QStringLiteral("pic_paste,-pic_text"), items),
                 (QStringList{"pic_paste", "pic_changed", "pic_translate"}));
        QCOMPARE(serialize(c, items), QStringLiteral("pic_paste,-pic_text,pic_changed,pic_translate"));
        QVERIFY(resolve(serialize(c, items), items) == c); // round trip
    }

    void everythingHidden()
    {
        const QList<Item> items = pictureItems();
        QList<Choice> c = resolve(QString(), items);
        for (Choice& x : c) {
            x.shown = false;
        }
        const QString s = serialize(c, items);
        QVERIFY(!s.isEmpty());
        QVERIFY(shownIds(s, items).isEmpty());
    }

    void mnemonicsStayUnique()
    {
        QCOMPARE(uniqueMnemonics({"&Alpha", "B&eta", "&aardvark", "plain", "Tr&ee"}),
                 (QStringList{"&Alpha", "B&eta", "aardvark", "plain", "Tree"}));
        QCOMPARE(plain(QStringLiteral("Tran&slate && more")), QStringLiteral("Translate & more"));
    }
};

QTEST_GUILESS_MAIN(TestPasteAs)
#include "tst_pasteas.moc"
