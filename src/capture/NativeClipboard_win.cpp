#include "NativeClipboard.h"

#include "ClipFilter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <iterator>

namespace clipitto::capture {

namespace {

// OpenClipboard/CloseClipboard as a scope.
class OpenedClipboard {
public:
    OpenedClipboard() : m_open(OpenClipboard(nullptr) != 0) {}
    ~OpenedClipboard()
    {
        if (m_open) {
            CloseClipboard();
        }
    }
    OpenedClipboard(const OpenedClipboard&) = delete;
    OpenedClipboard& operator=(const OpenedClipboard&) = delete;
    bool isOpen() const { return m_open; }

private:
    bool m_open;
};

// GlobalLock/GlobalUnlock as a scope.
class LockedMemory {
public:
    explicit LockedMemory(HANDLE handle)
        : m_handle(static_cast<HGLOBAL>(handle))
        , m_data(m_handle ? GlobalLock(m_handle) : nullptr)
        , m_size(m_data ? GlobalSize(m_handle) : 0)
    {
    }
    ~LockedMemory()
    {
        if (m_data) {
            GlobalUnlock(m_handle);
        }
    }
    LockedMemory(const LockedMemory&) = delete;
    LockedMemory& operator=(const LockedMemory&) = delete;
    const void* data() const { return m_data; }
    SIZE_T size() const { return m_size; }

private:
    HGLOBAL m_handle;
    void* m_data;
    SIZE_T m_size;
};

QString formatName(UINT format)
{
    switch (format) {
    case CF_TEXT:
        return QStringLiteral("CF_TEXT");
    case CF_OEMTEXT:
        return QStringLiteral("CF_OEMTEXT");
    case CF_UNICODETEXT:
        return QStringLiteral("text/plain"); // what Qt calls it
    case CF_LOCALE:
        return QStringLiteral("CF_LOCALE");
    default:
        break;
    }
    wchar_t name[256];
    const int length = GetClipboardFormatNameW(format, name, int(std::size(name)));
    return length > 0 ? QString::fromWCharArray(name, length) : QStringLiteral("#%1").arg(format);
}

// A DWORD-valued marker format set to 0 means "don't keep this" (password managers). A
// marker whose value can't be read counts as 0: when in doubt, don't record.
bool markerIsZero(UINT format)
{
    const LockedMemory memory(GetClipboardData(format));
    return !memory.data() || memory.size() < sizeof(DWORD) || *static_cast<const DWORD*>(memory.data()) == 0;
}

// The largest picture copied out of the clipboard, as the source app encoded it.
constexpr SIZE_T kMaxPictureBytes = 512 * 1024 * 1024;

// The device-independent bitmap (CF_DIB: a BITMAPINFOHEADER, colors, pixels) as a BMP file:
// the same bytes behind a BITMAPFILEHEADER, which says where the pixels start.
QByteArray dibAsBmp(const void* dib, SIZE_T size)
{
    if (size < sizeof(BITMAPINFOHEADER)) {
        return {};
    }
    const auto* info = static_cast<const BITMAPINFOHEADER*>(dib);
    DWORD colors = info->biClrUsed;
    if (colors == 0 && info->biBitCount <= 8) {
        colors = 1u << info->biBitCount;
    }
    DWORD headerBytes = info->biSize + colors * sizeof(RGBQUAD);
    if (info->biSize == sizeof(BITMAPINFOHEADER) && info->biCompression == BI_BITFIELDS) {
        headerBytes += 3 * sizeof(DWORD); // the color masks follow a plain BITMAPINFOHEADER
    }
    if (headerBytes > size) {
        return {};
    }
    BITMAPFILEHEADER file = {};
    file.bfType = 0x4d42; // "BM"
    file.bfSize = DWORD(sizeof(file) + size);
    file.bfOffBits = DWORD(sizeof(file) + headerBytes);
    QByteArray bmp(reinterpret_cast<const char*>(&file), sizeof(file));
    bmp.append(static_cast<const char*>(dib), qsizetype(size));
    return bmp;
}

// Reads the picture of a copy without text: PNG if offered (keeps transparency), else the DIB.
NativeClipboardRead::Status readPicture(NativeClipboardRead& result)
{
    const UINT png = RegisterClipboardFormatW(L"PNG");
    const UINT format = IsClipboardFormatAvailable(png) ? png : (IsClipboardFormatAvailable(CF_DIB) ? CF_DIB : 0);
    if (format == 0) {
        return NativeClipboardRead::Status::NoText;
    }
    const LockedMemory memory(GetClipboardData(format));
    if (!memory.data()) {
        return NativeClipboardRead::Status::Busy; // listed but not rendered yet
    }
    if (memory.size() > kMaxPictureBytes) {
        return NativeClipboardRead::Status::NoText; // far over any limit: not worth copying
    }
    if (format == png) {
        result.image = QByteArray(static_cast<const char*>(memory.data()), qsizetype(memory.size()));
        result.imageType = QStringLiteral("image/png");
    } else {
        result.image = dibAsBmp(memory.data(), memory.size());
        result.imageType = QStringLiteral("image/bmp");
    }
    return result.image.isEmpty() ? NativeClipboardRead::Status::NoText : NativeClipboardRead::Status::Ok;
}

} // namespace

NativeClipboardRead readNativeClipboard()
{
    NativeClipboardRead result;
    const OpenedClipboard clipboard;
    if (!clipboard.isOpen()) {
        result.status = NativeClipboardRead::Status::Busy; // another program has it open
        return result;
    }

    bool hasUnicodeText = false;
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        const QString name = formatName(format);
        if (format == CF_UNICODETEXT) {
            hasUnicodeText = true;
        } else if (name == QLatin1String("CanIncludeInClipboardHistory") || name == QLatin1String("CanUploadToCloudClipboard")) {
            if (markerIsZero(format)) {
                result.formats << name + QStringLiteral("=0");
            }
            continue;
        }
        result.formats << name;
    }
    if (ClipFilter::isSensitive(result.formats)) {
        // A password manager asked not to be recorded: report the markers (the pipeline
        // rejects the copy) without ever reading or rendering the secret.
        result.status = NativeClipboardRead::Status::Ok;
        return result;
    }
    if (!hasUnicodeText) {
        // Windows offers CF_UNICODETEXT for every text copy (converting CF_TEXT if needed):
        // without it, this is a picture or something we don't record.
        result.status = readPicture(result);
        return result;
    }

    const LockedMemory memory(GetClipboardData(CF_UNICODETEXT));
    if (!memory.data() || memory.size() < sizeof(wchar_t)) {
        // Listed but not delivered: the source app hasn't rendered it yet. Never fall back
        // to CF_TEXT, which loses every character outside the ANSI code page.
        result.status = NativeClipboardRead::Status::Busy;
        return result;
    }
    const auto* text = static_cast<const wchar_t*>(memory.data());
    const std::size_t length = wcsnlen(text, memory.size() / sizeof(wchar_t));
    result.text = QString::fromWCharArray(text, qsizetype(length));
    result.text.replace(QLatin1String("\r\n"), QLatin1String("\n")); // as Qt does: one entry per text
    result.status = NativeClipboardRead::Status::Ok;
    return result;
}

} // namespace clipitto::capture
