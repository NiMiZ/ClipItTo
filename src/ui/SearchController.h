#pragma once

#include <QObject>
#include <QTimer>

namespace clipitto {
struct Config;
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

class HistoryModel;

// Turns the search text into model rows. Non-empty queries are debounced; clearing the
// query switches back to the default view (saved items + recent copies) immediately.
class SearchController : public QObject {
    Q_OBJECT

public:
    SearchController(db::Repository& repo, HistoryModel& model, QObject* parent = nullptr);

    void applyConfig(const Config& config);

    void setQuery(const QString& query);
    QString query() const { return m_query; }
    void refresh(); // re-run the current query now
    bool isPending() const { return m_debounce.isActive(); }

    int queriesRun() const { return m_queriesRun; } // for tests and diagnostics

    static constexpr int searchLimit = 100;

signals:
    void updated(); // the model was repopulated

private:
    void run();

    db::Repository& m_repo;
    HistoryModel& m_model;
    QTimer m_debounce;
    QString m_query;
    int m_recentCount = 20;
    int m_savedCap = 15;
    bool m_showSavedOnTop = true;
    int m_queriesRun = 0;
};

} // namespace clipitto::ui
