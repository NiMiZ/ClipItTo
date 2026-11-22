#include "SearchHelp.h"

#include <QCoreApplication>

#define HELP_TR(s) QCoreApplication::translate("clipitto::searchhelp", s)

namespace clipitto::searchhelp {

bool isHelpQuery(const QString& query)
{
    return query.trimmed() == QLatin1String("/?");
}

QString passwordPrefix()
{
    return QStringLiteral("pw:");
}

std::vector<Row> rows(const QString& passwordKeyword)
{
    std::vector<Row> r;
    const auto add = [&r](Kind kind, const QString& prefix, const QString& example, const QString& meaning) {
        r.push_back({kind, prefix, example, meaning});
    };
    // Filters: one row per key SearchFilter::parse knows ("image:" is the same as "img:").
    add(Kind::Filter, QStringLiteral("img:"), QStringLiteral("img:"), HELP_TR("pictures only"));
    add(Kind::Filter, QStringLiteral("text:"), QStringLiteral("text:"), HELP_TR("text copies only (no pictures, no passwords)"));
    add(Kind::Filter, QStringLiteral("pin:"), QStringLiteral("pin:"), HELP_TR("pinned copies"));
    add(Kind::Filter, QStringLiteral("star:"), QStringLiteral("star:"), HELP_TR("starred copies"));
    add(Kind::Filter, QStringLiteral("group:"), QStringLiteral("group:work"),
        HELP_TR("copies in a group whose name contains the word (\"group:\" alone: in any group)"));
    add(Kind::Filter, QStringLiteral("note:"), QStringLiteral("note:todo"),
        HELP_TR("copies whose note contains the word (\"note:\" alone: has a note)"));
    add(Kind::Filter, QStringLiteral("today:"), QStringLiteral("today:"), HELP_TR("copied or used today"));
    add(Kind::Filter, QStringLiteral("since:"), QStringLiteral("since:3d"),
        HELP_TR("copied or used in the last 30m, 2h, 3d, 2w, or since yesterday or a date like 2026-10-01"));
    add(Kind::Regex, QStringLiteral("re:"), QStringLiteral("re:^https?://.*\\.pdf$"),
        HELP_TR("regular expression: ignores case, stops after 0.3 s; one per search"));
    add(Kind::Password, passwordPrefix(), QStringLiteral("pw: hunter"),
        HELP_TR("search inside passwords; what you type is hidden. Unlocked: part of a password; locked: only a whole "
                "one. Other searches never look inside passwords. Filters are ignored here"));
    if (!passwordKeyword.isEmpty() && passwordKeyword != passwordPrefix()) {
        add(Kind::Password, passwordKeyword, passwordKeyword + QStringLiteral(" hunter"),
            HELP_TR("your own password search keyword (Settings): the same as pw:"));
    }
    add(Kind::Rule, QStringLiteral("\""), QStringLiteral("re:\"error \\d+ at\""),
        HELP_TR("quote a pattern to type spaces in it (or use \\s); filter values have no spaces"));
    add(Kind::Rule, QStringLiteral("(?-i)"), QStringLiteral("re:(?-i)^Error"), HELP_TR("at the start of a pattern: match case"));
    add(Kind::Rule, QStringLiteral("words"), QStringLiteral("star: budget since:1w"),
        HELP_TR("words and filters must all match; anything else with a colon (a URL, foo:bar) is a plain word"));
    add(Kind::Rule, QStringLiteral("/?"), QStringLiteral("/?"), HELP_TR("show this list"));
    return r;
}

QStringList prefixes(Kind kind, const QString& passwordKeyword)
{
    QStringList out;
    for (const Row& row : rows(passwordKeyword)) {
        if (row.kind == kind) {
            out << row.prefix;
        }
    }
    return out;
}

QString text(const QString& passwordKeyword)
{
    QStringList lines;
    lines << HELP_TR("What the search box understands (Esc closes):");
    for (const Row& row : rows(passwordKeyword)) {
        lines << QStringLiteral("%1   %2").arg(row.example, row.meaning);
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace clipitto::searchhelp
