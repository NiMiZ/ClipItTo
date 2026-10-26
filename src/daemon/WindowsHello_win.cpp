// Windows Hello through KeyCredentialManager (C++/WinRT). The key pair lives in the TPM (or
// Windows' software key store) and is used only after Windows Hello verifies the user. Its
// signature over a fixed challenge is the same every time (RSA, PKCS#1 v1.5), so hashed it
// gives a stable key that wraps the vault's secret key. C++/WinRT headers come first: Qt's
// headers define macros its code doesn't expect.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include "WindowsHello.h"

#include <QCoreApplication>
#include <QCryptographicHash>

#include <windows.h>

#include <atomic>
#include <thread>

namespace clipitto::hello {

namespace {

using winrt::Windows::Security::Credentials::KeyCredentialCreationOption;
using winrt::Windows::Security::Credentials::KeyCredentialManager;
using winrt::Windows::Security::Credentials::KeyCredentialStatus;
using winrt::Windows::Security::Cryptography::BinaryStringEncoding;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;

const wchar_t* const kKeyName = L"io.github.nimiz.ClipItTo";
const wchar_t* const kChallenge = L"ClipItTo password vault v1";

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::Daemon", text);
}

QString statusText(KeyCredentialStatus status)
{
    switch (status) {
    case KeyCredentialStatus::UserCanceled:
        return tr("cancelled");
    case KeyCredentialStatus::NotFound:
        return tr("ClipItTo's Windows Hello key is missing; turn Windows Hello off and on again in Settings");
    case KeyCredentialStatus::UserPrefersPassword:
        return tr("you chose the password instead");
    case KeyCredentialStatus::SecurityDeviceLocked:
        return tr("the security device is locked");
    default:
        return tr("Windows Hello failed (status %1)").arg(int(status));
    }
}

// A thread-local COM apartment for the WinRT calls (never the GUI thread's).
struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};

// The Windows Security prompt can open behind other windows when a background app asks:
// bring it to the front while waiting (as KeePassXC does).
class PromptRaiser {
public:
    PromptRaiser()
        : m_thread([this] {
            for (int i = 0; i < 50 && !m_done; ++i) {
                if (HWND prompt = FindWindowW(L"Credential Dialog Xaml Host", nullptr)) {
                    SetForegroundWindow(prompt);
                    return;
                }
                Sleep(100);
            }
        })
    {
    }
    ~PromptRaiser()
    {
        m_done = true;
        m_thread.join();
    }
    PromptRaiser(const PromptRaiser&) = delete;
    PromptRaiser& operator=(const PromptRaiser&) = delete;

private:
    std::atomic<bool> m_done{false};
    std::thread m_thread;
};

} // namespace

bool available(QString* why)
{
    try {
        const Apartment apartment;
        if (KeyCredentialManager::IsSupportedAsync().get()) {
            return true;
        }
        if (why) {
            *why = tr("Windows Hello is not set up on this computer (Settings › Accounts › Sign-in options).");
        }
    } catch (const winrt::hresult_error& e) {
        if (why) {
            *why = QString::fromWCharArray(e.message().c_str());
        }
    }
    return false;
}

QByteArray unlockKey(bool create, QString* error)
{
    try {
        const Apartment apartment;
        const PromptRaiser raiser;
        const auto retrieved = create ? KeyCredentialManager::RequestCreateAsync(
                                            kKeyName, KeyCredentialCreationOption::ReplaceExisting)
                                            .get()
                                      : KeyCredentialManager::OpenAsync(kKeyName).get();
        if (retrieved.Status() != KeyCredentialStatus::Success) {
            if (error) {
                *error = statusText(retrieved.Status());
            }
            return {};
        }
        const auto challenge = CryptographicBuffer::ConvertStringToBinary(kChallenge, BinaryStringEncoding::Utf8);
        const auto signed_ = retrieved.Credential().RequestSignAsync(challenge).get();
        if (signed_.Status() != KeyCredentialStatus::Success) {
            if (error) {
                *error = statusText(signed_.Status());
            }
            return {};
        }
        winrt::com_array<uint8_t> signature;
        CryptographicBuffer::CopyToByteArray(signed_.Result(), signature);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArrayView("clipitto-hello-v1"));
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(signature.data()), qsizetype(signature.size())));
        SecureZeroMemory(signature.data(), signature.size());
        return hash.result();
    } catch (const winrt::hresult_error& e) {
        if (error) {
            *error = QString::fromWCharArray(e.message().c_str());
        }
        return {};
    }
}

void removeKey()
{
    try {
        const Apartment apartment;
        KeyCredentialManager::DeleteAsync(kKeyName).get();
    } catch (const winrt::hresult_error&) {
        // Not there (never created, or deleted already): nothing to remove.
    }
}

} // namespace clipitto::hello
