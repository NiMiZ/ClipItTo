#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QWidget;

// Small modal questions, injectable so tests never block on a dialog.
namespace clipitto::ui {

// Free text. nullopt = cancelled.
using TextPrompt = std::function<std::optional<QString>(QWidget* parent, const QString& title,
                                                        const QString& label, const QString& current)>;
// Yes/no. true = confirmed.
using ConfirmPrompt = std::function<bool(QWidget* parent, const QString& question)>;

TextPrompt defaultTextPrompt();
// Picks a group from `choices` or takes a typed one ("" removes the group). Arrow keys step
// through the choices from the first one. nullopt = cancelled.
std::optional<QString> askGroup(QWidget* parent, const QString& current, const QStringList& choices);
ConfirmPrompt defaultConfirmPrompt();

} // namespace clipitto::ui
