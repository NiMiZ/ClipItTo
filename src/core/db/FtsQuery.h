#pragma once

#include <QString>
#include <QStringList>

// Turns raw user search input into safe SQLite query parameters.
namespace clipitto::db::fts {

// Splits on whitespace; empty input yields an empty list.
QStringList tokenize(const QString& query);

// The trigram tokenizer cannot match substrings shorter than 3 characters,
// so FTS is only usable when every token has at least 3 code points.
bool canUseTrigram(const QStringList& tokens);

// Each token becomes a quoted FTS5 phrase ("a" "b" = a AND b) with embedded quotes
// doubled, so operators like NEAR, *, -, ^ or column filters are matched literally.
QString matchExpression(const QStringList& tokens);

// "%token%" with %, _ and \ escaped; use with LIKE ... ESCAPE '\'.
QString likePattern(const QString& token);

} // namespace clipitto::db::fts
