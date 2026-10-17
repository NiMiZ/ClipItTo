#include "Prompts.h"

#include <QComboBox>
#include <QCoreApplication>
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

std::optional<QString> askGroup(QWidget* parent, const QString& current, const QStringList& choices)
{
    QInputDialog dialog(parent);
    // Translated as part of the popup, where this prompt used to live.
    dialog.setWindowTitle(QCoreApplication::translate("clipitto::ui::PopupWindow", "Assign group"));
    dialog.setLabelText(QCoreApplication::translate(
        "clipitto::ui::PopupWindow", "Group (use / for sub-groups, e.g. Work/Servers; empty to remove):"));
    dialog.setComboBoxItems(choices);
    dialog.setComboBoxEditable(true);
    dialog.setTextValue(current);
    if (!choices.contains(current)) {
        // The combo box selects the first choice by itself while showing other text, so Down
        // would skip that choice (and do nothing with a single group): select none instead.
        auto* combo = dialog.findChild<QComboBox*>();
        Q_ASSERT(combo); // QInputDialog's own; tst_popup notices if Qt ever changes this
        if (combo) {
            combo->setCurrentIndex(-1);
            combo->setEditText(current);
        }
    }
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.textValue();
}

ConfirmPrompt defaultConfirmPrompt()
{
    return [](QWidget* parent, const QString& question) {
        return QMessageBox::question(parent, QStringLiteral("ClipItTo"), question) == QMessageBox::Yes;
    };
}

} // namespace clipitto::ui
