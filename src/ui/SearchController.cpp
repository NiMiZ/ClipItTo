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

void SearchController::setSuspended(bool suspended)
{
    m_suspended = suspended;
    if (suspended) {
        m_debounce.stop();
    }
}

void SearchController::run()
{
    if (m_suspended) {
        return;
    }
    ++m_queriesRun;
    std::vector<HistoryModel::Section> sections;
    std::vector<HistoryModel::GroupData> groups;
    std::vector<Entry> grouped;
    std::vector<Entry> favorites;
    try {
        if (m_query.trimmed().isEmpty()) {
            for (const auto& g : m_repo.groupTree()) {
                groups.push_back({g.path, g.clipCount});
            }
            if (!groups.empty()) {
                grouped = m_repo.groupedEntries();
            }
            favorites = m_repo.favorites();
            auto pinned = m_showSavedOnTop ? m_repo.pinned(m_savedCap) : std::vector<Entry>{};
            // Recent is the copy history (starred and grouped copies too, also found in the
            // group tree); pinned copies are listed above it, so it leaves those out.
            auto recent = m_showSavedOnTop ? m_repo.recentUnpinned(m_recentCount)
                                           : m_repo.recent(m_recentCount, /*unsavedOnly=*/false);
            if (!pinned.empty()) {
                sections.push_back({tr("Pinned"), std::move(pinned)});
                // A header only separates something: with nothing pinned, recent stands alone.
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
    m_model.setContent(std::move(sections), groups, grouped, favorites);
    emit updated();
}

} // namespace clipitto::ui
