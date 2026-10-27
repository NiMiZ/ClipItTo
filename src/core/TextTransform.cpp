#include "TextTransform.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

namespace clipitto::texttransform {

namespace {

QString t(const char* text)
{
    return QCoreApplication::translate("clipitto::texttransform", text);
}

std::optional<QString> fail(QString* why, const QString& reason)
{
    if (why) {
        *why = reason;
    }
    return std::nullopt;
}

QString title(const QString& text)
{
    QString out = text.toLower();
    bool start = true;
    for (QChar& c : out) {
        if (c.isLetter()) {
            if (start) {
                c = c.toUpper();
            }
            start = false;
        } else if (!c.isDigit() && c != QLatin1Char('\'')) {
            start = true;
        } else {
            start = false;
        }
    }
    return out;
}

} // namespace

QList<Info> all()
{
    return {
        {Kind::Upper, t("&UPPER CASE")},
        {Kind::Lower, t("&lower case")},
        {Kind::Title, t("&Title Case")},
        {Kind::Trim, t("Tri&m white space at the ends")},
        {Kind::CollapseSpaces, t("Collapse &spaces")},
        {Kind::OneLine, t("&One line")},
        {Kind::JsonPretty, t("&JSON, pretty")},
        {Kind::JsonMinify, t("JSON, m&inified")},
        {Kind::UrlEncode, t("URL-&encode")},
        {Kind::UrlDecode, t("URL-&decode")},
        {Kind::Base64Encode, t("&Base64-encode")},
        {Kind::Base64Decode, t("Base64-de&code")},
    };
}

std::optional<QString> apply(Kind kind, const QString& text, QString* why)
{
    switch (kind) {
    case Kind::Upper:
        return text.toUpper();
    case Kind::Lower:
        return text.toLower();
    case Kind::Title:
        return title(text);
    case Kind::Trim:
        return text.trimmed();
    case Kind::CollapseSpaces: {
        static const QRegularExpression runs(QStringLiteral("[ \\t]+"));
        QString out = text;
        out.replace(runs, QStringLiteral(" "));
        return out;
    }
    case Kind::OneLine: {
        static const QRegularExpression breaks(QStringLiteral("\\s*[\\r\\n]+\\s*"));
        QString out = text.trimmed();
        out.replace(breaks, QStringLiteral(" "));
        return out;
    }
    case Kind::JsonPretty:
    case Kind::JsonMinify: {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            return fail(why, t("Not valid JSON: %1").arg(error.errorString()));
        }
        const auto format = kind == Kind::JsonPretty ? QJsonDocument::Indented : QJsonDocument::Compact;
        return QString::fromUtf8(doc.toJson(format)).trimmed();
    }
    case Kind::UrlEncode:
        return QString::fromLatin1(QUrl::toPercentEncoding(text));
    case Kind::UrlDecode: {
        const QByteArray bytes = QByteArray::fromPercentEncoding(text.toUtf8());
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(bytes);
        if (decoder.hasError()) {
            return fail(why, t("The decoded bytes are not valid text."));
        }
        return decoded;
    }
    case Kind::Base64Encode:
        return QString::fromLatin1(text.toUtf8().toBase64());
    case Kind::Base64Decode: {
        const auto decoded = QByteArray::fromBase64Encoding(text.trimmed().toUtf8(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return fail(why, t("Not valid Base64."));
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString out = decoder(decoded.decoded);
        if (decoder.hasError()) {
            return fail(why, t("The decoded bytes are not valid text."));
        }
        return out;
    }
    }
    return fail(why, QString());
}

} // namespace clipitto::texttransform
