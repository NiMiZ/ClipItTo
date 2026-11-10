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

// Ctrl+E: the note of a copy and, for a plain-text copy, its text. `text` is nullopt for a picture
// or a password copy (the dialog then has only the Note box). `apply` is called when the user
// saves; it returns an error to show in the dialog (which then stays open, nothing changed) or an
// empty string once saved. The prompt returns true when something was saved.
struct CopyEdit {
    QString note;
    std::optional<QString> text; // nullopt: no text box
};
using EditApply = std::function<QString(const CopyEdit&)>;
using EditPrompt = std::function<bool(QWidget* parent, const QString& note, const std::optional<QString>& text,
                                      const EditApply& apply)>;
// Object names the dialog's widgets carry ("editNoteBox", "editTextBox", "editError"), for tests.
EditPrompt defaultEditPrompt();

// A masked password (the master password). nullopt = cancelled. The caller wipes the text.
using PasswordPrompt = std::function<std::optional<QString>(QWidget* parent, const QString& label)>;
PasswordPrompt defaultPasswordPrompt();

// A name for a new password copy. `suggestion` (the app it was copied from) is shown as grey
// placeholder text that goes away when the user types; empty input keeps it as the name.
// nullopt = cancelled.
using NamePrompt = std::function<std::optional<QString>(QWidget* parent, const QString& suggestion)>;
NamePrompt defaultNamePrompt();
// True when the last default name box closed because nobody answered within 60 seconds.
bool lastNamePromptTimedOut();

// Setting or changing the master password: masked fields only, the new one typed twice.
struct MasterPasswordAnswer {
    QString current; // empty when setting a first one
    QString chosen;
};
using MasterPasswordPrompt = std::function<std::optional<MasterPasswordAnswer>(QWidget* parent, bool change)>;
MasterPasswordPrompt defaultMasterPasswordPrompt();

// Shows a copy's text as a QR code to scan with a phone (modal, closes with Esc). `text` is never logged.
using QrView = std::function<void(QWidget* parent, const QString& text)>;
QrView defaultQrView();

} // namespace clipitto::ui
