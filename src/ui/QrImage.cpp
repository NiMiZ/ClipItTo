#include "QrImage.h"

#include "qrcodegen.hpp"

#include <QCoreApplication>

#include <optional>
#include <stdexcept>

namespace clipitto::ui {

QImage qrImage(const QString& text, int minSide, QString* error)
{
    const auto fail = [&](const char* message) {
        if (error) {
            *error = QCoreApplication::translate("clipitto::ui::QrImage", message);
        }
        return QImage();
    };
    if (text.isEmpty()) {
        return fail("There is nothing to show.");
    }
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() > kQrMaxBytes) {
        return fail("This copy is too long for a QR code (about 2900 characters at most).");
    }
    struct Wipe { // the text is not left behind in freed memory
        QByteArray& bytes;
        ~Wipe() { bytes.fill(0); }
    } wipe{const_cast<QByteArray&>(utf8)};
    try {
        // Medium error correction when it fits (a scan survives a smudge); the lowest when only that holds it.
        std::optional<qrcodegen::QrCode> encoded;
        try {
            encoded.emplace(qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM));
        } catch (const std::length_error&) {
            encoded.emplace(qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::LOW));
        }
        const qrcodegen::QrCode& code = *encoded;
        constexpr int kQuiet = 4;
        const int modules = code.getSize() + 2 * kQuiet;
        const int scale = qMax(1, (minSide + modules - 1) / modules);
        QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < code.getSize(); ++y) {
            for (int x = 0; x < code.getSize(); ++x) {
                if (!code.getModule(x, y)) {
                    continue;
                }
                for (int dy = 0; dy < scale; ++dy) {
                    QRgb* row = reinterpret_cast<QRgb*>(image.scanLine((y + kQuiet) * scale + dy));
                    for (int dx = 0; dx < scale; ++dx) {
                        row[(x + kQuiet) * scale + dx] = qRgb(0, 0, 0);
                    }
                }
            }
        }
        return image;
    } catch (const std::length_error&) {
        return fail("This copy is too long for a QR code (about 2900 characters at most).");
    } catch (const std::exception&) { // qrcodegen::data_too_long is a std::length_error; anything else is unexpected
        return fail("A QR code could not be made from this copy.");
    }
}

} // namespace clipitto::ui
