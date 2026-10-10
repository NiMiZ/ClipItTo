#include "SearchController.h"

#include "Config.h"
#include "HistoryModel.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSearch, "clipitto.search")

namespace clipitto::ui {

SearchController::SearchController(db::Repository& repo, HistoryModel& model, QObject* parent)
    : QObject(parent)
    , m_repo(repo)
    , m_model(model)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, &SearchController::run);
}

void SearchController::applyConfig(const Config& config)
{
    m_recentCount = config.recentCount;
    m_savedCap = config.savedCap;
    m_showSavedOnTop = config.showSavedOnTop;
    m_debounce.setInterval(config.searchDebounceMs);
}

void SearchController::setQuery(const QString& query)
{
    m_query = query;
    if (m_query.trimmed().isEmpty()) {
        m_debounce.stop();
        run();
    } else {
        m_debounce.start();
    }
}

void SearchController::refresh()
{
    m_debounce.stop();
    run();
}

void SearchController::run()
{
    ++m_queriesRun;
    std::vector<HistoryModel::Row> rows;
    try {
        if (m_query.trimmed().isEmpty()) {
            const auto saved = m_showSavedOnTop ? m_repo.saved(m_savedCap) : std::vector<Entry>{};
            const auto recent = m_repo.recent(m_recentCount, /*unsavedOnly=*/m_showSavedOnTop);
            if (!saved.empty()) {
                rows.push_back({tr("Saved"), {}});
                for (const Entry& e : saved) {
                    rows.push_back({{}, e});
                }
                if (!recent.empty()) {
                    rows.push_back({tr("Recent"), {}});
                }
            }
            for (const Entry& e : recent) {
                rows.push_back({{}, e});
            }
        } else {
            for (const Entry& e : m_repo.search(m_query, searchLimit)) {
                rows.push_back({{}, e});
            }
        }
    } catch (const db::DbError& e) {
        qCWarning(lcSearch) << "query failed:" << e.what();
    }
    m_model.setRows(std::move(rows));
    emit updated();
}

} // namespace clipitto::ui
