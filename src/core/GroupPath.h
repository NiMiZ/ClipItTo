#pragma once

#include <QString>

// Groups form a tree addressed by paths: "Work/Servers" is Servers inside Work.
namespace clipitto::groups {

inline constexpr QChar kSeparator = QLatin1Char('/');

// Trims each level and drops empty ones; "/" and the display separator "›" both split.
// "  Work / Servers " -> "Work/Servers"; blank input -> empty string.
QString normalize(const QString& path);

QString parent(const QString& path); // "Work/Servers" -> "Work"; top level -> ""
QString leaf(const QString& path); // "Work/Servers" -> "Servers"
int depth(const QString& path); // "Work" -> 0, "Work/Servers" -> 1

// True if `path` is `ancestor` itself or lies anywhere below it.
bool isWithin(const QString& path, const QString& ancestor);

QString display(const QString& path); // "Work › Servers"

} // namespace clipitto::groups
