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
    m_model.setPreviewLines(config.previewLines);
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
    std::vector<HistoryModel::Section> sections;
    std::vector<HistoryModel::GroupData> groups;
    std::vector<Entry> grouped;
    try {
        if (m_query.trimmed().isEmpty()) {
            for (const auto& g : m_repo.groupTree()) {
                groups.push_back({g.path, g.clipCount});
            }
            if (!groups.empty()) {
                grouped = m_repo.groupedEntries();
            }
            auto saved = m_showSavedOnTop ? m_repo.saved(m_savedCap) : std::vector<Entry>{};
            auto recent = m_repo.recent(m_recentCount, /*unsavedOnly=*/m_showSavedOnTop);
            if (!saved.empty()) {
                sections.push_back({tr("Saved"), std::move(saved)});
                // A header only separates something: with nothing saved, recent stands alone.
                sections.push_back({recent.empty() ? QString() : tr("Recent"), std::move(recent)});
            } else {
                sections.push_back({QString(), std::move(recent)});
            }
        } else {
            sections.push_back({QString(), m_repo.search(m_query, searchLimit)});
        }
    } catch (const db::DbError& e) {
        qCWarning(lcSearch) << "query failed:" << e.what();
    }
    m_model.setContent(std::move(sections), groups, grouped);
    emit updated();
}

} // namespace clipitto::ui
