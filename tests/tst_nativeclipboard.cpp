// The Windows native clipboard reader against the real clipboard. It replaces what is on
// the clipboard, so it only runs when CLIPITTO_TEST_REAL_CLIPBOARD is set (CI sets it).
#include "NativeClipboard.h"

#include <QBuffer>
#include <QImage>
#include <QTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

using clipitto::capture::NativeClipboardRead;
using clipitto::capture::readNativeClipboard;

namespace {

// Puts `bytes` on the clipboard as `format`, replacing everything.
bool setClipboard(UINT format, const QByteArray& bytes)
{
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(bytes.size()));
    std::memcpy(GlobalLock(memory), bytes.constData(), size_t(bytes.size()));
    GlobalUnlock(memory);
    const bool ok = SetClipboardData(format, memory) != nullptr; // the clipboard owns it now
    CloseClipboard();
    return ok;
}

// A 3x2, 24-bit bottom-up DIB: red, green, blue on the top row, white below.
QByteArray dib()
{
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = 3;
    info.biHeight = 2;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    QByteArray bytes(reinterpret_cast<const char*>(&info), sizeof(info));
    const auto row = [](std::initializer_list<QRgb> pixels) {
        QByteArray r;
        for (QRgb p : pixels) {
            r.append(char(qBlue(p))).append(char(qGreen(p))).append(char(qRed(p)));
        }
        r.append(QByteArray(3, '\0')); // rows are padded to 4 bytes: 9 -> 12
        return r;
    };
    bytes += row({qRgb(255, 255, 255), qRgb(255, 255, 255), qRgb(255, 255, 255)}); // bottom row first
    bytes += row({qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)});
    return bytes;
}

} // namespace

class TestNativeClipboard : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsEmpty("CLIPITTO_TEST_REAL_CLIPBOARD")) {
            QSKIP("uses the real clipboard: set CLIPITTO_TEST_REAL_CLIPBOARD=1 to run");
        }
    }

    void bitmapIsReadAsBmp()
    {
        QVERIFY(setClipboard(CF_DIB, dib()));
        const NativeClipboardRead read = readNativeClipboard();
        QCOMPARE(int(read.status), int(NativeClipboardRead::Status::Ok));
        QVERIFY(read.text.isEmpty());
        QCOMPARE(read.imageType, QStringLiteral("image/bmp"));
        const QImage image = QImage::fromData(read.image, "BMP");
        QCOMPARE(image.size(), QSize(3, 2));
        QCOMPARE(image.pixelColor(0, 0), QColor(255, 0, 0));
        QCOMPARE(image.pixelColor(2, 0), QColor(0, 0, 255));
        QCOMPARE(image.pixelColor(1, 1), QColor(255, 255, 255));
    }

    void pngFormatIsPreferredAndKeptAsIs()
    {
        QImage picture(5, 4, QImage::Format_ARGB32);
        picture.fill(QColor(10, 20, 30, 128)); // transparency: why PNG is preferred
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        QVERIFY(setClipboard(RegisterClipboardFormatW(L"PNG"), png));
        const NativeClipboardRead read = readNativeClipboard();
        QCOMPARE(int(read.status), int(NativeClipboardRead::Status::Ok));
        QCOMPARE(read.imageType, QStringLiteral("image/png"));
        QCOMPARE(read.image, png);
    }

    void textStillWins()
    {
        const QString text = QStringLiteral("שלום");
        QVERIFY(setClipboard(CF_UNICODETEXT,
                             QByteArray(reinterpret_cast<const char*>(text.utf16()), (text.size() + 1) * 2)));
        const NativeClipboardRead read = readNativeClipboard();
        QCOMPARE(read.text, text);
        QVERIFY(read.image.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestNativeClipboard)
#include "tst_nativeclipboard.moc"
