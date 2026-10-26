// A ClipItTo key in the Windows Credential Manager (generic credential, per user, this
// machine). Wide (W) APIs only; UTF-16 <-> UTF-8 conversion stays inside this file.
#include "SystemKeyStore.h"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace clipitto {

namespace {

QString lastErrorText(const char* what)
{
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    QString message = length ? QString::fromWCharArray(buffer, int(length)).trimmed()
                             : QStringLiteral("error %1").arg(code);
    LocalFree(buffer);
    return QStringLiteral("%1: %2").arg(QLatin1String(what), message);
}

// RAII for the buffer CredReadW allocates.
struct CredentialPtr {
    PCREDENTIALW p = nullptr;
    ~CredentialPtr()
    {
        if (p) {
            SecureZeroMemory(p->CredentialBlob, p->CredentialBlobSize);
            CredFree(p);
        }
    }
};

class WindowsKeyStore : public KeyStore {
public:
    explicit WindowsKeyStore(const QString& entry)
        : m_target((QStringLiteral("io.github.nimiz.ClipItTo/") + entry).toStdWString())
        , m_comment((QStringLiteral("ClipItTo ") + entry).toStdWString())
    {
    }

    bool available(QString*) override { return true; } // Credential Manager is part of Windows

    std::optional<QByteArray> read(QString* error) override
    {
        CredentialPtr cred;
        if (!CredReadW(m_target.c_str(), CRED_TYPE_GENERIC, 0, &cred.p)) {
            if (GetLastError() == ERROR_NOT_FOUND) {
                if (error) {
                    error->clear();
                }
            } else if (error) {
                *error = lastErrorText("CredReadW");
            }
            return std::nullopt;
        }
        return QByteArray(reinterpret_cast<const char*>(cred.p->CredentialBlob),
                          int(cred.p->CredentialBlobSize));
    }

    bool write(const QByteArray& key, QString* error) override
    {
        CREDENTIALW cred = {};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<wchar_t*>(m_target.c_str());
        cred.Comment = const_cast<wchar_t*>(m_comment.c_str());
        cred.CredentialBlobSize = DWORD(key.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.constData()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE; // survives logoff; stays on this machine
        cred.UserName = const_cast<wchar_t*>(L"clipitto");
        if (!CredWriteW(&cred, 0)) {
            if (error) {
                *error = lastErrorText("CredWriteW");
            }
            return false;
        }
        return true;
    }

    bool remove(QString* error) override
    {
        if (!CredDeleteW(m_target.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
            if (error) {
                *error = lastErrorText("CredDeleteW");
            }
            return false;
        }
        return true;
    }

private:
    std::wstring m_target; // io.github.nimiz.ClipItTo/<entry>; history-key as before
    std::wstring m_comment;
};

} // namespace

std::shared_ptr<KeyStore> createSystemKeyStore(const QString& entry)
{
    return std::make_shared<WindowsKeyStore>(entry);
}

} // namespace clipitto
