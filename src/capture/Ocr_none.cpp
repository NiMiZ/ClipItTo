#include "Ocr.h"

#include <QCoreApplication>

namespace clipitto::ocr {

bool builtIn()
{
    return false;
}

bool available(QString* why)
{
    if (why) {
        *why = QCoreApplication::translate("clipitto::ocr", "This build has no text recognition.");
    }
    return false;
}

std::optional<QString> recognize(const QByteArray&, QString* error)
{
    available(error);
    return std::nullopt;
}

} // namespace clipitto::ocr
