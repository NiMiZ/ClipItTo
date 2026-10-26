#include "SourceApp.h"

#include <QFileInfo>
#include <QHash>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace clipitto::capture {

namespace {

class ProcessHandle {
public:
    explicit ProcessHandle(DWORD pid) : m_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {}
    ~ProcessHandle()
    {
        if (m_handle) {
            CloseHandle(m_handle);
        }
    }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

QString imagePath(HWND window)
{
    DWORD pid = 0;
    if (!window || GetWindowThreadProcessId(window, &pid) == 0 || pid == 0 || pid == GetCurrentProcessId()) {
        return {};
    }
    const ProcessHandle process(pid);
    if (!process.get()) {
        return {};
    }
    std::vector<wchar_t> path(MAX_PATH);
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD size = DWORD(path.size());
        if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) {
            return QString::fromWCharArray(path.data(), int(size));
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return {};
        }
        path.resize(path.size() * 4);
    }
    return {};
}

// The FileDescription from the executable's version resource ("KeePass Password Safe").
QString fileDescription(const QString& path)
{
    const std::wstring file = path.toStdWString();
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) {
        return {};
    }
    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)
        || length < sizeof(Translation)) {
        return {};
    }
    // Some executables list a translation without a matching string block: try each, then
    // the usual US English ones.
    std::vector<Translation> candidates(translations, translations + length / sizeof(Translation));
    candidates.push_back({0x0409, 0x04B0});
    candidates.push_back({0x0409, 0x04E4});
    for (const Translation& t : candidates) {
        wchar_t key[64];
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", t.language, t.codePage);
        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &valueLength) && valueLength > 0) {
            const QString description = QString::fromWCharArray(value).trimmed();
            if (!description.isEmpty()) {
                return description;
            }
        }
    }
    return {};
}

} // namespace

QString clipboardSourceApp()
{
    // Some apps put data on the clipboard without an owner window: then the foreground window,
    // where the copy was just made.
    QString path = imagePath(GetClipboardOwner());
    if (path.isEmpty()) {
        path = imagePath(GetForegroundWindow());
    }
    if (path.isEmpty()) {
        return {};
    }
    // The version resource is read from the executable on disk: once per app (this runs on
    // the GUI thread, for each password copy).
    static QHash<QString, QString> names;
    auto it = names.constFind(path);
    if (it == names.constEnd()) {
        const QString description = fileDescription(path);
        it = names.insert(path, description.isEmpty() ? QFileInfo(path).completeBaseName() : description);
    }
    return *it;
}

} // namespace clipitto::capture
