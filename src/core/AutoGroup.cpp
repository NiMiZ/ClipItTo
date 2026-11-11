#include "AutoGroup.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

#include <cstring>

namespace clipitto::autogroup {

namespace {

qint64 hashOf(const QString& kind, const QString& value)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(kind.toUtf8());
    h.addData(QByteArrayView("\0", 1));
    h.addData(value.toUtf8());
    const QByteArray digest = h.result();
    qint64 out = 0;
    std::memcpy(&out, digest.constData(), sizeof out);
    return out;
}

bool isStopWord(const QString& w)
{
    static const QSet<QString> words{
        QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("for"), QStringLiteral("that"),
        QStringLiteral("this"), QStringLiteral("with"), QStringLiteral("from"), QStringLiteral("have"),
        QStringLiteral("not"), QStringLiteral("are"), QStringLiteral("was"), QStringLiteral("you"),
        QStringLiteral("your"), QStringLiteral("will"), QStringLiteral("can"), QStringLiteral("http"),
        QStringLiteral("https"), QStringLiteral("www"), QStringLiteral("com")};
    return words.contains(w);
}

} // namespace

std::vector<Token> tokens(const QString& full)
{
    std::vector<Token> out;
    QSet<qint64> seen;
    const auto add = [&](const QString& kind, const QString& value, int weight) {
        if (value.isEmpty() || int(out.size()) >= kMaxTokens) {
            return;
        }
        const qint64 h = hashOf(kind, value);
        if (!seen.contains(h)) {
            seen.insert(h);
            out.push_back({h, weight});
        }
    };

    const QString text = full.left(kMaxChars).trimmed();
    if (text.isEmpty()) {
        return out;
    }
    const bool oneLine = !text.contains(QLatin1Char('\n'));

    // Shapes first, so the token cap never drops them.
    static const QRegularExpression url(
        QStringLiteral(R"(^[a-zA-Z][a-zA-Z0-9+.-]*://(?:[^/@\s]*@)?([^/:?#\s]+)(?::\d+)?(/[^/?#\s]*)?)"));
    static const QRegularExpression pathLike(QStringLiteral(R"(^(?:~|[A-Za-z]:|\\\\|/)[\\/]?[^\s]*[\\/][^\s]*$)"));
    static const QRegularExpression command(QStringLiteral(R"(^(?:sudo\s+)?([a-z][a-z0-9._-]{1,30})\s+\S)"));
    if (const auto m = url.match(text); oneLine && m.hasMatch()) {
        QString host = m.captured(1).toLower();
        if (host.startsWith(QLatin1String("www."))) {
            host.remove(0, 4);
        }
        add(QStringLiteral("host"), host, 4);
        const QStringList labels = host.split(QLatin1Char('.'));
        if (labels.size() > 2) {
            add(QStringLiteral("dom"), labels.mid(labels.size() - 2).join(QLatin1Char('.')), 2);
        }
        const QString folder = m.captured(2).toLower();
        if (folder.size() > 1) {
            add(QStringLiteral("pf"), host + folder, 3);
        }
    } else if (oneLine && pathLike.match(text).hasMatch()) {
        QString p = text;
        p.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            add(QStringLiteral("top"), parts.first().toLower(), 3);
            add(QStringLiteral("dir"), parts.at(parts.size() - 2).toLower(), 3);
            if (parts.size() >= 3) {
                add(QStringLiteral("dir2"), parts.mid(0, parts.size() - 1).join(QLatin1Char('/')).toLower(), 3);
            }
        }
    } else if (oneLine) {
        if (const auto m = command.match(text); m.hasMatch() && !isStopWord(m.captured(1))) {
            add(QStringLiteral("cmd"), m.captured(1), 4);
        }
    }

    // Words: runs of letters and digits, 3 to 24 long, not only digits.
    int start = -1;
    const int n = int(text.size());
    for (int i = 0; i <= n && int(out.size()) < kMaxTokens; ++i) {
        const bool wordChar = i < n && text.at(i).isLetterOrNumber();
        if (wordChar) {
            if (start < 0) {
                start = i;
            }
            continue;
        }
        if (start >= 0) {
            const int len = i - start;
            if (len >= 3 && len <= 24) {
                const QString word = text.mid(start, len).toLower();
                bool digitsOnly = true;
                for (const QChar c : word) {
                    digitsOnly = digitsOnly && c.isDigit();
                }
                if (!digitsOnly && !isStopWord(word)) {
                    add(QStringLiteral("w"), word, 1);
                }
            }
            start = -1;
        }
    }
    return out;
}

void Model::add(const QString& group, qint64 token, int count)
{
    if (count <= 0) {
        return;
    }
    int index = int(m_groups.indexOf(group));
    if (index < 0) {
        index = int(m_groups.size());
        m_groups << group;
    }
    Entry& e = m_tokens[token];
    e.hits.push_back({index, count});
    e.total += count;
}

QString Model::classify(const QString& text) const
{
    return classify(tokens(text));
}

QString Model::classify(const std::vector<Token>& tokens, double* score) const
{
    if (m_groups.isEmpty() || tokens.empty()) {
        return {};
    }
    std::vector<double> scores(std::size_t(m_groups.size()), 0.0);
    for (const Token& t : tokens) {
        const auto it = m_tokens.find(t.hash);
        if (it == m_tokens.end()) {
            continue;
        }
        const Entry& e = it->second;
        for (const Hit& hit : e.hits) {
            // The group's share of this token, damped when it was seen only once or twice.
            const double share = double(hit.count) / double(e.total);
            const double sure = double(hit.count) / (double(hit.count) + 0.5);
            scores[std::size_t(hit.group)] += t.weight * share * sure;
        }
    }
    int best = -1;
    double top = 0;
    double second = 0;
    for (int i = 0; i < int(scores.size()); ++i) {
        const double s = scores[std::size_t(i)];
        if (s > top) {
            second = top;
            top = s;
            best = i;
        } else if (s > second) {
            second = s;
        }
    }
    if (score) {
        *score = top;
    }
    if (best < 0 || top < kThreshold || top < kMargin * second) {
        return {};
    }
    return m_groups.at(best);
}

} // namespace clipitto::autogroup
