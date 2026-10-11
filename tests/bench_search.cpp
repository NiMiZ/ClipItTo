#include "TestDb.h"

#include <QRandomGenerator>
#include <QTest>

// Search latency over a realistic history size. Target: < 5 ms per query.
class BenchSearch : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        static const QStringList words = {
            QStringLiteral("kubectl"), QStringLiteral("deploy"), QStringLiteral("SELECT"), QStringLiteral("users"),
            QStringLiteral("https://example.com/path"), QStringLiteral("docker"), QStringLiteral("compose"),
            QStringLiteral("git"), QStringLiteral("commit"), QStringLiteral("lorem"), QStringLiteral("ipsum"),
            QStringLiteral("function"), QStringLiteral("return"), QStringLiteral("const"), QStringLiteral("ssh"),
        };
        QRandomGenerator rng(42);
        clipitto::db::Transaction tx(m_t.db);
        for (int i = 0; i < 5000; ++i) {
            QStringList line;
            const int n = 5 + int(rng.bounded(40));
            for (int w = 0; w < n; ++w) {
                line << words[int(rng.bounded(words.size()))];
            }
            line << QString::number(i);
            const auto id = m_t.repo->ingest(line.join(QLatin1Char(' '))).id;
            if (i % 50 == 0) {
                m_t.repo->setFavorite(id, true);
            }
        }
        tx.commit();
    }

    void search_data()
    {
        QTest::addColumn<QString>("query");
        QTest::newRow("fts single") << QStringLiteral("deploy");
        QTest::newRow("fts multi") << QStringLiteral("docker commit");
        QTest::newRow("fts rare") << QStringLiteral("4999");
        QTest::newRow("like short") << QStringLiteral("gi");
    }

    void search()
    {
        QFETCH(QString, query);
        QBENCHMARK {
            m_t.repo->search(query, 100);
        }
    }

    void emptyQueryView()
    {
        QBENCHMARK {
            m_t.repo->pinned(15);
            m_t.repo->favorites();
            m_t.repo->recent(20, true);
        }
    }

private:
    TestDb m_t;
};

QTEST_GUILESS_MAIN(BenchSearch)
#include "bench_search.moc"
