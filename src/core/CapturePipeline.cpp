#include "CapturePipeline.h"

#include "RichText.h"
#include "SecretScan.h"
#include "PasswordVault.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QtConcurrent>

#include <utility>

Q_LOGGING_CATEGORY(lcCapture, "clipitto.capture")

namespace clipitto {

CapturePipeline::CapturePipeline(db::Repository& repo, int maxHistory, qint64 maxItemBytes, QObject* parent)
    : QObject(parent)
    , m_repo(repo)
    , m_filter(maxItemBytes)
    , m_maxHistory(maxHistory)
{
}

void CapturePipeline::setLimits(int maxHistory, qint64 maxItemBytes)
{
    m_maxHistory = maxHistory;
    m_filter.setMaxBytes(maxItemBytes);
}

void CapturePipeline::setImageLimits(int maxImages, qint64 maxImageBytes)
{
    m_maxImages = qMax(0, maxImages);
    m_filter.setMaxImageBytes(maxImageBytes);
    if (m_maxImages == 0) {
        m_waitingImages.clear(); // turned off: don't store what was waiting either
    }
}

void CapturePipeline::setKeepFormatting(bool on)
{
    m_keepFormatting = on;
    if (!on) {
        for (Text& waiting : m_waiting) {
            waiting.html.clear(); // turned off while paused: what is queued keeps no formatted version either
        }
    }
}

void CapturePipeline::setAutoGrouping(bool on)
{
    m_autoGroup = on;
    m_repo.setAutoGrouping(on);
}

void CapturePipeline::setPasswords(const PasswordVault* vault, bool keep, const QString& group)
{
    m_vault = vault;
    m_keepPasswords = keep;
    m_passwordGroup = group;
}

bool CapturePipeline::keepsPasswords() const
{
    return m_keepPasswords && m_vault && m_vault->isSetUp();
}

void CapturePipeline::setPaused(bool paused)
{
    m_paused = paused;
    if (paused) {
        return;
    }
    const QList<Text> waiting = std::exchange(m_waiting, {});
    for (const Text& copy : waiting) {
        store(copy);
    }
    const QList<SealedCopy> secrets = std::exchange(m_waitingSecrets, {});
    for (const SealedCopy& copy : secrets) {
        storeSecret(copy);
    }
    const QList<ImageClip> images = std::exchange(m_waitingImages, {});
    for (const ImageClip& image : images) {
        storeImage(image);
    }
}

void CapturePipeline::processImage(const ImageClip& image, const QStringList& mimeTypes)
{
    if (m_maxImages == 0) {
        return; // pictures are turned off
    }
    const ClipFilter::Verdict verdict = m_filter.checkImage(image, mimeTypes);
    if (verdict != ClipFilter::Verdict::Accept) {
        qCDebug(lcCapture) << "skipped image, verdict" << int(verdict) << "bytes" << image.png.size();
        emit rejected(verdict);
        return;
    }
    if (m_paused) {
        constexpr qsizetype kMaxWaitingImages = 2; // pictures are big, and held unencrypted here
        m_waitingImages << image;
        while (m_waitingImages.size() > kMaxWaitingImages) {
            m_waitingImages.removeFirst();
        }
        return;
    }
    storeImage(image);
}

void CapturePipeline::storeImage(const ImageClip& image)
{
    try {
        const auto result = m_repo.ingestImage(image);
        if (result.inserted) {
            m_repo.pruneImages(m_maxImages);
        }
        emit stored(result.id, result.inserted);
    } catch (const db::DbError& e) {
        qCWarning(lcCapture) << "failed to store image:" << e.what();
    }
}

void CapturePipeline::process(const QString& text, const QStringList& mimeTypes, const QString& html)
{
    ClipFilter::Verdict verdict = m_filter.check(text, mimeTypes);
    // A key or token is never given a formatted version, whether it is kept as a password copy or not.
    const bool looksSecret = verdict == ClipFilter::Verdict::Accept
        && ((m_detectSecrets && !secretscan::kindOf(text).isEmpty())
            || (m_detectPasswords && secretscan::looksLikeGeneratedPassword(text))
            || passwordrules::matchesAny(m_passwordRules, text));
    QString detected; // what an unmarked copy looks like, when it is kept as a password anyway
    if (verdict == ClipFilter::Verdict::Accept && (m_detectSecrets || m_detectPasswords || !m_passwordRules.isEmpty())
        && keepsPasswords()) {
        if (m_detectSecrets) {
            detected = secretscan::kindOf(text);
        }
        if (detected.isEmpty() && passwordrules::matchesAny(m_passwordRules, text)) {
            detected = QStringLiteral("Matches your password rule");
        }
        if (detected.isEmpty() && m_detectPasswords && secretscan::looksLikeGeneratedPassword(text)) {
            detected = QStringLiteral("Looks like a password"); // stored as is, like the kinds from secretscan::kindOf
        }
        if (detected.isEmpty() && m_detectPasswords && text.size() >= 8 && text.size() <= 64
            && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'))) {
            // Say why a short single-line copy was not taken (the class of reason only, never the text). Debug level: it is
            // written for every such copy, so with the daemon's --verbose off nothing about when copies were made is kept.
            qCDebug(lcCapture).noquote() << "detect_passwords: not taken:" << secretscan::whyNotGeneratedPassword(text);
        }
        if (!detected.isEmpty()) {
            verdict = ClipFilter::Verdict::Sensitive; // an unmarked key, token or password: kept like a password copy
        }
    }
    if (verdict == ClipFilter::Verdict::Sensitive && m_keepPasswords && !keepsPasswords()) {
        // Backends don't even read the text then; say why nothing was kept.
        emit rejected(verdict);
        emit passwordSkipped();
        return;
    }
    if (verdict == ClipFilter::Verdict::Sensitive && keepsPasswords() && !text.trimmed().isEmpty()
        && text.toUtf8().size() > m_filter.maxBytes()) {
        emit rejected(ClipFilter::Verdict::TooLarge); // the same cap as any copy (max_item_bytes)
        return;
    }
    if (verdict == ClipFilter::Verdict::Sensitive && keepsPasswords() && !text.trimmed().isEmpty()) {
        // Sealed right away: the password is never stored, queued or logged in clear.
        QByteArray plain = text.toUtf8();
        QString note = ClipFilter::sourceApp(mimeTypes);
        if (note.isEmpty()) {
            note = detected;
        }
        SealedCopy copy{m_vault->seal(plain), m_vault->match(plain), note};
        PasswordVault::wipe(plain);
        if (m_paused) {
            m_waitingSecrets << copy;
            while (m_waitingSecrets.size() > m_maxHistory) {
                m_waitingSecrets.removeFirst();
            }
            return;
        }
        storeSecret(copy);
        return;
    }
    if (verdict != ClipFilter::Verdict::Accept) {
        qCDebug(lcCapture) << "skipped clip, verdict" << int(verdict);
        emit rejected(verdict);
        return;
    }
    // Only a version that says more than the text, and fits the same size cap as a copy.
    QString rich;
    if (m_keepFormatting && !looksSecret && !html.isEmpty() && html.toUtf8().size() <= m_filter.maxBytes()
        && richtext::hasFormatting(html) && richtext::matchesText(html, text)) {
        rich = html;
    }
    if (m_paused) {
        m_waiting.removeIf([&](const Text& w) { return w.text == text; }); // the same text copied twice is one entry anyway
        constexpr qsizetype kMaxWaitingRich = 50; // HTML can be large and is held in memory while paused
        m_waiting << Text{text, m_waiting.size() < kMaxWaitingRich ? rich : QString(), looksSecret};
        while (m_waiting.size() > m_maxHistory) {
            m_waiting.removeFirst(); // keep memory bounded: the oldest would be pruned anyway
        }
        return;
    }
    store({text, rich, looksSecret});
}

void CapturePipeline::storeSecret(const SealedCopy& copy)
{
    try {
        const auto result = m_repo.ingestSecret(copy.sealed, copy.match, copy.note, m_passwordGroup);
        if (result.inserted) {
            m_repo.prune(m_maxHistory);
        }
        emit stored(result.id, result.inserted);
        if (result.inserted) {
            emit passwordStored(result.id, copy.note);
        }
    } catch (const db::DbError& e) {
        qCWarning(lcCapture) << "failed to store a password copy:" << e.what();
    }
}

void CapturePipeline::store(const Text& copy)
{
    try {
        const auto result = m_repo.ingest(copy.text);
        // The newest copy of this text decides: with formatting now, or plain this time.
        if (copy.html.isEmpty()) {
            if (!result.inserted) {
                m_repo.clearRich(result.id);
            }
        } else {
            m_repo.setRich(result.id, copy.html);
        }
        if (result.inserted) {
            // The prune query is index-backed and cheap, so the limit is kept exact.
            m_repo.prune(m_maxHistory);
        }
        emit stored(result.id, result.inserted);
        if (result.inserted && m_autoGroup && !copy.secretLike) {
            autoFile(result.id, copy.text);
        }
    } catch (const db::DbError& e) {
        qCWarning(lcCapture) << "failed to store clip:" << e.what();
    }
}

void CapturePipeline::autoFile(qint64 id, const QString& text)
{
    const std::shared_ptr<const autogroup::Model> model = m_repo.autoModel(); // cached; rebuilt only after learning
    if (!model || model->empty()) {
        return;
    }
    ++m_autoPending;
    const quint64 forgets = m_repo.autoForgets();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, id, forgets] {
        const QString group = watcher->result();
        watcher->deleteLater();
        --m_autoPending;
        if (group.isEmpty() || !m_autoGroup || m_paused || forgets != m_repo.autoForgets()) {
            return; // no clear match (or turned off meanwhile): the copy stays in Recent
        }
        try {
            if (m_repo.setAutoGroup(id, group)) {
                qCDebug(lcCapture) << "auto-filed copy" << id;
                emit autoFiled(id, group);
            }
        } catch (const db::DbError& e) {
            qCWarning(lcCapture) << "failed to file a copy automatically:" << e.what();
        }
    });
    // The worker only reads its own copy of the text and the immutable snapshot.
    watcher->setFuture(QtConcurrent::run([model, text] { return model->classify(text); }));
}

} // namespace clipitto
