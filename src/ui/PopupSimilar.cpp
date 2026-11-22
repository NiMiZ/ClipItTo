// "Show similar copies" and "Keep this one, delete the others" (roadmap 22). Part of PopupWindow; kept in its own file.
// Privacy: only counts are logged, never a copy's text. Passwords and pictures are never read or compared.
#include "PopupSimilar.h"

#include "HistoryView.h"
#include "PlatformKeys.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "Similar.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QLoggingCategory>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <iterator>

Q_LOGGING_CATEGORY(lcSimilar, "clipitto.popup.similar")

namespace clipitto::ui {

using Kind = HistoryModel::Kind;

namespace {

constexpr int kSliceMs = 10; // one slice of scanning, then the event loop gets the thread back
constexpr int kPageSize = 100;

// Pinned, starred, grouped or with a note: the user chose to keep it.
bool isProtected(const Entry& e)
{
    return e.pinned() || e.favorite || !e.group.isEmpty() || !e.note.isEmpty();
}

} // namespace

bool PopupWindow::similarScanning() const
{
    return m_similar && !m_similar->finished;
}

void PopupWindow::showSimilar(qint64 id)
{
    if (isBusy()) {
        return;
    }
    std::optional<Entry> reference;
    try {
        reference = m_repo.get(id); // read again: it may have become a password meanwhile
    } catch (const db::DbError& err) {
        qCWarning(lcSimilar) << "reading a copy failed:" << err.what();
        return;
    }
    if (!reference || reference->secret || reference->image) {
        return;
    }
    if (similar::normalise(reference->content).isEmpty()) {
        flash(tr("This copy has no text to compare."));
        return;
    }
    endSimilar(false);
    if (m_search->isPasswordMode()) {
        m_search->leavePasswordMode();
    }
    m_search->clear();
    m_controller->setSuspended(true); // the list is ours until the view is left
    m_similar = std::make_unique<SimilarScan>(*reference);
    m_similar->shown.push_back(*reference);
    m_keepOneButton->setVisible(true);
    m_keepOneButton->setEnabled(false);
    showSimilarRows();
    m_view->setFocus();
    m_view->selectFirstEntry(m_startFocus);
    m_similarTimer->start();
}

void PopupWindow::similarStep()
{
    if (!m_similar) {
        return;
    }
    SimilarScan& scan = *m_similar;
    QElapsedTimer slice;
    slice.start();
    const std::size_t before = scan.shown.size();
    try {
        while (!scan.finished) {
            if (scan.scanMs + slice.elapsed() >= m_similarBudgetMs) {
                scan.outOfTime = true;
                scan.finished = true;
                break;
            }
            if (slice.elapsed() >= kSliceMs) {
                break;
            }
            if (scan.pos >= scan.page.size()) {
                if (scan.offset >= similar::kMaxCopies) {
                    scan.capped = true;
                    scan.finished = true;
                    break;
                }
                scan.page = m_repo.similarPage(scan.offset, std::min(kPageSize, similar::kMaxCopies - scan.offset),
                                               similar::kMaxChars);
                scan.pos = 0;
                if (scan.page.empty()) {
                    scan.finished = true;
                    break;
                }
                scan.offset += int(scan.page.size());
                continue;
            }
            const Entry& candidate = scan.page[scan.pos++];
            ++scan.compared;
            if (candidate.id == scan.refId || !scan.ref.matches(candidate.content)) {
                continue;
            }
            if (auto whole = m_repo.get(candidate.id); whole && !whole->secret && !whole->image) {
                scan.shown.push_back(std::move(*whole));
            }
        }
    } catch (const db::DbError& err) {
        qCWarning(lcSimilar) << "scan failed:" << err.what();
        scan.finished = true;
    }
    scan.scanMs += slice.elapsed();
    if (scan.shown.size() != before || scan.finished) {
        showSimilarRows();
    }
    if (scan.finished) {
        for (Entry& e : scan.page) { // the texts read for comparing are not kept
            e.content.fill(QLatin1Char('\0'));
        }
        scan.page.clear();
        qCDebug(lcSimilar) << "scan over:" << scan.compared << "compared," << (scan.shown.size() - 1)
                           << "similar, out of time:" << scan.outOfTime;
        m_keepOneButton->setEnabled(scan.shown.size() > 1);
    } else {
        m_similarTimer->start();
    }
}

void PopupWindow::showSimilarRows()
{
    if (!m_similar) {
        return;
    }
    const SimilarScan& scan = *m_similar;
    const Selection current = selectionOf(m_view->currentIndex());
    std::vector<Entry> rows = scan.shown;
    m_model->setContent({{tr("Similar copies"), std::move(rows)}});
    m_view->setEmptyText(tr("No similar copies."));
    if (current.kind == Kind::Entry && current.id != 0) {
        const QModelIndex again = m_model->findEntry(current.id);
        if (again.isValid()) {
            m_view->selectIndex(again);
        }
    }
    const int found = int(scan.shown.size()) - 1;
    QString text;
    if (!scan.finished) {
        text = tr("Looking for similar copies… %1 found so far").arg(found);
    } else if (found == 0) {
        text = tr("No similar copies.");
    } else {
        text = found == 1 ? tr("1 similar copy.") : tr("%1 similar copies.").arg(found);
    }
    if (scan.outOfTime) {
        text += tr(" Stopped after %1 ms: only the %2 newest copies were checked.").arg(m_similarBudgetMs).arg(scan.compared);
    } else if (scan.capped) {
        text += tr(" Only the newest %1 copies are checked.").arg(similar::kMaxCopies);
    }
    if (scan.finished) {
        text += tr(" Esc goes back.");
    }
    m_footer->setText(text);
}

void PopupWindow::endSimilar(bool refreshList)
{
    if (!m_similar) {
        return;
    }
    m_similarTimer->stop();
    for (Entry& e : m_similar->shown) { // the texts of the view are overwritten before they are freed
        e.content.fill(QLatin1Char('\0'));
    }
    for (Entry& e : m_similar->page) {
        e.content.fill(QLatin1Char('\0'));
    }
    m_similar.reset();
    m_keepOneButton->setVisible(false);
    if (!m_undo) {
        m_footer->setText(m_footerHint);
    }
    if (!isBusy()) {
        m_controller->setSuspended(false);
        if (refreshList) {
            m_controller->refresh();
        }
    }
}

void PopupWindow::keepOnlyThis(qint64 keepId)
{
    if (!m_similar || similarScanning() || isBusy()) {
        return;
    }
    std::vector<qint64> doomed;
    int kept = 0; // pinned, starred, grouped or noted: never deleted
    try {
        bool keepIsHere = false;
        for (const Entry& shown : m_similar->shown) {
            if (shown.id == keepId) {
                keepIsHere = true;
                continue;
            }
            const std::optional<Entry> fresh = m_repo.get(shown.id); // as it is now
            if (!fresh || fresh->secret || fresh->image) {
                continue;
            }
            if (isProtected(*fresh)) {
                ++kept;
            } else {
                doomed.push_back(fresh->id);
            }
        }
        if (!keepIsHere) {
            return;
        }
    } catch (const db::DbError& err) {
        qCWarning(lcSimilar) << "reading the copies failed:" << err.what();
        return;
    }
    if (doomed.empty()) {
        showMessage(kept > 0 ? tr("Nothing to delete: the other %1 are pinned, starred, in a group or have a note.").arg(kept)
                             : tr("There are no other copies to delete."));
        return;
    }
    const int count = int(doomed.size());
    QString question = count == 1 ? tr("Delete 1 similar copy and keep this one?")
                                  : tr("Delete %1 similar copies and keep this one?").arg(count);
    if (kept > 0) {
        question += kept == 1 ? tr("\n\n1 pinned, starred, grouped or noted copy is not deleted.")
                              : tr("\n\n%1 pinned, starred, grouped or noted copies are not deleted.").arg(kept);
    }
    bool confirmed = false;
    ++m_modalDepth;
    confirmed = m_confirm(this, question);
    --m_modalDepth;
    if (!confirmed) {
        return;
    }
    endUndo(); // an earlier Undo is over once something else is deleted
    std::vector<db::Repository::Removed> removed;
    try {
        for (const qint64 id : doomed) {
            std::optional<db::Repository::Removed> snap = m_repo.snapshot(id);
            // Checked again now: the copy may have been pinned, noted or made a password since the question.
            if (!snap || isProtected(snap->entry) || snap->entry.secret || snap->entry.image) {
                continue;
            }
            m_repo.remove(id);
            removed.push_back(std::move(*snap));
        }
    } catch (const db::DbError& err) {
        qCWarning(lcSimilar) << "deleting a copy failed:" << err.what();
    }
    qCDebug(lcSimilar) << "deleted" << removed.size() << "similar copies," << kept << "kept";
    if (removed.empty()) {
        return;
    }
    QString notice = removed.size() == 1 ? tr("Deleted 1 similar copy.")
                                         : tr("Deleted %1 similar copies.").arg(removed.size());
    if (kept > 0) {
        notice += kept == 1 ? tr(" 1 kept (pinned, starred, grouped or noted).")
                            : tr(" %1 kept (pinned, starred, grouped or noted).").arg(kept);
    }
    notice += tr(" Undo: Ctrl+Shift+Z");
    for (const db::Repository::Removed& r : removed) {
        for (Entry& e : m_similar->shown) {
            if (e.id == r.entry.id) {
                e.content.fill(QLatin1Char('\0'));
            }
        }
        std::erase_if(m_similar->shown, [&](const Entry& e) { return e.id == r.entry.id; });
    }
    m_undo = std::move(removed.front());
    m_undoMore.assign(std::make_move_iterator(removed.begin() + 1), std::make_move_iterator(removed.end()));
    m_undoNotice = notice;
    m_undoTimer->start();

    // The view shows what is left (the kept copy first).
    std::vector<Entry>& shown = m_similar->shown;
    const auto keepAt = std::find_if(shown.begin(), shown.end(), [&](const Entry& e) { return e.id == keepId; });
    if (keepAt != shown.begin() && keepAt != shown.end()) {
        std::rotate(shown.begin(), keepAt, keepAt + 1);
    }
    showSimilarRows();
    showUndoNotice();
    m_keepOneButton->setEnabled(false); // what is left is protected
}

} // namespace clipitto::ui
