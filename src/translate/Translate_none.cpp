#include "Translate.h"

#include <QCoreApplication>

namespace clipitto::translate {

bool builtIn()
{
    return false;
}

std::optional<QString> translate(const Pack&, const QString&, QString* error)
{
    if (error) {
        *error = QCoreApplication::translate("clipitto::translate", "This build has no translation.");
    }
    return std::nullopt;
}

} // namespace clipitto::translate
