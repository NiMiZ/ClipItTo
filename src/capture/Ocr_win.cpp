// Text recognition with Windows.Media.Ocr (C++/WinRT): built into Windows 10 and later, offline,
// reads the languages the user has installed (Settings > Time & language). C++/WinRT headers come
// first: Qt's headers define macros its code doesn't expect.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include "Ocr.h"

#include <QCoreApplication>
#include <QStringList>

namespace clipitto::ocr {

namespace {

using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapDecoder;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;

struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment()
    {
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }
};

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::ocr", text);
}

QString noLanguage()
{
    return tr("Windows has no text-recognition language installed (Settings > Time & language > Language).");
}

} // namespace

bool available(QString* why)
{
    try {
        const Apartment apartment;
        if (OcrEngine::TryCreateFromUserProfileLanguages()) {
            return true;
        }
        if (why) {
            *why = noLanguage();
        }
    } catch (const winrt::hresult_error& e) {
        if (why) {
            *why = QString::fromWCharArray(e.message().c_str());
        }
    } catch (...) {
        if (why) {
            *why = tr("Text recognition could not be checked.");
        }
    }
    return false;
}

std::optional<QString> recognize(const QByteArray& png, QString* error)
{
    const auto fail = [error](const QString& text) {
        if (error) {
            *error = text;
        }
        return std::optional<QString>();
    };
    try {
        const Apartment apartment;
        const OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            return fail(noLanguage());
        }
        // The PNG bytes into a stream Windows can decode.
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(png.constData()),
                                                           reinterpret_cast<const uint8_t*>(png.constData()) + png.size()));
        writer.StoreAsync().get();
        writer.DetachStream();
        stream.Seek(0);
        const BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
        if (decoder.PixelWidth() > OcrEngine::MaxImageDimension() || decoder.PixelHeight() > OcrEngine::MaxImageDimension()) {
            return fail(tr("The picture is too large to read."));
        }
        const auto bitmap = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied).get();
        const auto result = engine.RecognizeAsync(bitmap).get();
        QStringList lines;
        for (const auto& line : result.Lines()) {
            lines << QString::fromWCharArray(line.Text().c_str());
        }
        return lines.join(QLatin1Char('\n')).trimmed();
    } catch (const winrt::hresult_error& e) {
        return fail(QString::fromWCharArray(e.message().c_str()));
    } catch (...) {
        return fail(tr("Text recognition failed."));
    }
}

} // namespace clipitto::ocr
