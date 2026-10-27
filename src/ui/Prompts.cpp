#include "Prompts.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

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

PasswordPrompt defaultPasswordPrompt()
{
    return [](QWidget* parent, const QString& label) -> std::optional<QString> {
        QInputDialog dialog(parent);
        dialog.setWindowTitle(QCoreApplication::translate("clipitto::ui::PopupWindow", "Unlock passwords"));
        dialog.setLabelText(label);
        dialog.setTextEchoMode(QLineEdit::Password);
        dialog.setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhHiddenText);
        if (dialog.exec() != QDialog::Accepted) {
            dialog.setTextValue(QString());
            return std::nullopt;
        }
        std::optional<QString> password = dialog.textValue();
        dialog.setTextValue(QString()); // the dialog's own copy
        return password;
    };
}

NamePrompt defaultNamePrompt()
{
    return [](QWidget* parent, const QString& suggestion) -> std::optional<QString> {
        const auto tr = [](const char* text) { return QCoreApplication::translate("clipitto::ui::PopupWindow", text); };
        QDialog dialog(parent);
        dialog.setWindowTitle(tr("Name this password"));
        auto* label = new QLabel(tr("A new password was copied. Name it so you can find it:"), &dialog);
        label->setWordWrap(true);
        auto* edit = new QLineEdit(&dialog);
        edit->setPlaceholderText(suggestion); // grey; gone as soon as something is typed
        edit->setInputMethodHints(Qt::ImhNoPredictiveText);
        auto* hint = new QLabel(suggestion.isEmpty() ? QString()
                                                     : tr("Left empty, it is named after the app it came from."),
                                &dialog);
        hint->setEnabled(false); // dim
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(label);
        layout->addWidget(edit);
        layout->addWidget(hint);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.setMinimumWidth(380);
        edit->setFocus();
        if (dialog.exec() != QDialog::Accepted) {
            return std::nullopt;
        }
        return edit->text();
    };
}

MasterPasswordPrompt defaultMasterPasswordPrompt()
{
    return [](QWidget* parent, bool change) -> std::optional<MasterPasswordAnswer> {
        const auto tr = [](const char* text) { return QCoreApplication::translate("clipitto::ui::SettingsDialog", text); };
        QDialog dialog(parent);
        dialog.setWindowTitle(change ? tr("Change the master password") : tr("Set a master password"));
        auto* form = new QFormLayout;
        const auto field = [&dialog] {
            auto* edit = new QLineEdit(&dialog);
            edit->setEchoMode(QLineEdit::Password);
            edit->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhHiddenText);
            return edit;
        };
        QLineEdit* current = change ? field() : nullptr;
        QLineEdit* chosen = field();
        QLineEdit* repeat = field();
        if (current) {
            form->addRow(tr("Current master password:"), current);
        }
        form->addRow(tr("New master password:"), chosen);
        form->addRow(tr("Repeat it:"), repeat);
        auto* note = new QLabel(tr("It is never stored or shown. If you forget it, your password copies can't "
                                   "be recovered. A few unrelated words make a strong one."),
                                &dialog);
        note->setWordWrap(true);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        auto* layout = new QVBoxLayout(&dialog);
        layout->addLayout(form);
        layout->addWidget(note);
        layout->addWidget(buttons);
        QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
        const auto update = [=] {
            const bool longEnough = chosen->text().size() >= kMinMasterPasswordLength;
            ok->setEnabled(longEnough && chosen->text() == repeat->text() && (!current || !current->text().isEmpty()));
            ok->setToolTip(longEnough ? QString() : tr("At least 8 characters"));
        };
        for (QLineEdit* edit : {current, chosen, repeat}) {
            if (edit) {
                QObject::connect(edit, &QLineEdit::textChanged, &dialog, update);
            }
        }
        update();
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        std::optional<MasterPasswordAnswer> answer;
        if (dialog.exec() == QDialog::Accepted) {
            answer = MasterPasswordAnswer{current ? current->text() : QString(), chosen->text()};
        }
        for (QLineEdit* edit : {current, chosen, repeat}) {
            if (edit) {
                edit->clear(); // the fields' own copies
            }
        }
        return answer;
    };
}

} // namespace clipitto::ui
