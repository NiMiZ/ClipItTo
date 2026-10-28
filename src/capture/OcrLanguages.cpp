#include "Ocr.h"

#include <QRegularExpression>

namespace clipitto::ocr {

QStringList parseLanguages(const QString& text)
{
    return text.split(QRegularExpression(QStringLiteral("[,+\\s]+")), Qt::SkipEmptyParts);
}

} // namespace clipitto::ocr
