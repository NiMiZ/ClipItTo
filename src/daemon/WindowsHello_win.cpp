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
#include <mutex>
#include <optional>
#include <thread>

namespace clipitto::hello {

namespace {

using winrt::Windows::Security::Credentials::KeyCredentialCreationOption;
using winrt::Windows::Security::Credentials::KeyCredentialManager;
using winrt::Windows::Security::Credentials::KeyCredentialStatus;
using winrt::Windows::Security::Cryptography::BinaryStringEncoding;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using winrt::Windows::Foundation::IAsyncInfo;

// The operation waiting for the user, so quitting can cancel it (see cancelPending()).
std::mutex g_pendingMutex;
std::optional<IAsyncInfo> g_pending;

// Waits for `operation` while it can be cancelled from another thread.
template <typename Operation>
auto waitFor(Operation operation)
{
    {
        const std::lock_guard lock(g_pendingMutex);
        g_pending = operation.template as<IAsyncInfo>();
    }
    struct Clear {
        ~Clear()
        {
            const std::lock_guard lock(g_pendingMutex);
            g_pending.reset();
        }
    } clear;
    return operation.get();
}

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

// A COM apartment for the WinRT calls on this (pool) thread, never the GUI thread's. The
// factory cache is cleared before leaving it: cached factories belong to the apartment.
struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    // No winrt::clear_factory_cache(): it is process-wide and unsafe while another thread (text
    // recognition's) is using WinRT.
    ~Apartment() { winrt::uninit_apartment(); }
};

// The Windows Security prompt can open behind other windows when a background app asks:
// bring it to the front while waiting (as KeePassXC does).
class PromptRaiser {
public:
    PromptRaiser()
        : m_thread([this] {
            // Windows may refuse the first tries (foreground lock): keep at it for a while.
            for (int i = 0; i < 50 && !m_done; ++i) {
                HWND prompt = FindWindowW(L"Credential Dialog Xaml Host", nullptr);
                if (prompt && GetForegroundWindow() == prompt) {
                    return;
                }
                if (prompt) {
                    SetForegroundWindow(prompt);
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

QString displayName()
{
    return QStringLiteral("Windows Hello");
}

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
    } catch (...) {
        if (why) {
            *why = tr("Windows Hello could not be checked");
        }
    }
    return false;
}

QByteArray unlockKey(bool create, QString* error)
{
    try {
        const Apartment apartment;
        const PromptRaiser raiser;
        const auto retrieved = create ? waitFor(KeyCredentialManager::RequestCreateAsync(
                                            kKeyName, KeyCredentialCreationOption::ReplaceExisting))
                                      : waitFor(KeyCredentialManager::OpenAsync(kKeyName));
        if (retrieved.Status() != KeyCredentialStatus::Success) {
            if (error) {
                *error = statusText(retrieved.Status());
            }
            return {};
        }
        const auto challenge = CryptographicBuffer::ConvertStringToBinary(kChallenge, BinaryStringEncoding::Utf8);
        const auto signed_ = waitFor(retrieved.Credential().RequestSignAsync(challenge));
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
    } catch (const winrt::hresult_canceled&) {
        if (error) {
            *error = tr("cancelled");
        }
        return {};
    } catch (const winrt::hresult_error& e) {
        if (error) {
            *error = QString::fromWCharArray(e.message().c_str());
        }
        return {};
    } catch (...) { // never into QtConcurrent: its result would rethrow on the GUI thread
        if (error) {
            *error = tr("Windows Hello failed");
        }
        return {};
    }
}

void removeKey()
{
    try {
        const Apartment apartment;
        KeyCredentialManager::DeleteAsync(kKeyName).get();
    } catch (...) {
        // Not there (never created, or deleted already): nothing to remove.
    }
}

void cancelPending()
{
    const std::lock_guard lock(g_pendingMutex);
    if (g_pending) {
        try {
            g_pending->Cancel();
        } catch (...) {
        }
    }
}

} // namespace clipitto::hello
