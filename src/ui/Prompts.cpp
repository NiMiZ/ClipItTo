#include "Prompts.h"

#include <QInputDialog>
#include <QMessageBox>

namespace clipitto::ui {

TextPrompt defaultTextPrompt()
{
    return [](QWidget* parent, const QString& title, const QString& label, const QString& current) {
        bool ok = false;
        const QString text = QInputDialog::getText(parent, title, label, QLineEdit::Normal, current, &ok);
        return ok ? std::optional<QString>(text) : std::nullopt;
    };
}

ConfirmPrompt defaultConfirmPrompt()
{
    return [](QWidget* parent, const QString& question) {
        return QMessageBox::question(parent, QStringLiteral("ClipItTo"), question) == QMessageBox::Yes;
    };
}

} // namespace clipitto::ui
