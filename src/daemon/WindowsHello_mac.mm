// Touch ID as the macOS counterpart of Windows Hello (see WindowsHello.h).
//
// A random 32-byte key wraps the vault's secret key, as the hashed Hello signature does on
// Windows. It lives in the login Keychain (generic password item, entry "touch-id-key"), and
// ClipItTo reads it only after LocalAuthentication has confirmed a fingerprint. That makes
// Touch ID a gate in front of the key, not a key only the Secure Enclave can release: a
// Keychain item bound to biometrics needs a signed app with a keychain access group, and
// ClipItTo is signed ad hoc. Anything running as you could read the item without the
// fingerprint, which the Settings text says.
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>

#include "PasswordVault.h"
#include "SystemKeyStore.h"
#include "WindowsHello.h"

#include <QCoreApplication>

#include <mutex>

namespace clipitto::hello {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::Daemon", text);
}

constexpr int kKeySize = 32;

std::mutex g_mutex;
LAContext* g_pending = nil; // the prompt that is waiting, so cancelPending() can end it

std::shared_ptr<KeyStore> keyStore()
{
    return createSystemKeyStore(QStringLiteral("touch-id-key"));
}

// Whether Touch ID can be used; the reason when not.
bool canUse(QString* why)
{
    @autoreleasepool {
        LAContext* context = [[LAContext alloc] init];
        NSError* error = nil;
        if ([context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&error]) {
            return true;
        }
        if (why) {
            switch (error ? error.code : 0) {
            case LAErrorBiometryNotEnrolled:
                *why = tr("No fingerprint is set up (System Settings › Touch ID & Password).");
                break;
            case LAErrorBiometryNotAvailable:
                *why = tr("This Mac has no Touch ID, or it is not available.");
                break;
            case LAErrorBiometryLockout:
                *why = tr("Touch ID is locked: unlock it with your login password first.");
                break;
            default:
                *why = error ? QString::fromNSString(error.localizedDescription) : tr("Touch ID is not available.");
                break;
            }
        }
        return false;
    }
}

// Asks for a fingerprint; blocks until answered. False with `error` when it wasn't.
bool askFingerprint(QString* error)
{
    @autoreleasepool {
        LAContext* context = [[LAContext alloc] init];
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending = context;
        }
        __block BOOL granted = NO;
        __block NSError* failure = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [context evaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
                localizedReason:tr("unlock your password copies").toNSString()
                          reply:^(BOOL success, NSError* e) {
                              granted = success;
                              failure = e;
                              dispatch_semaphore_signal(done);
                          }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending = nil;
        }
        if (!granted && error) {
            switch (failure ? failure.code : 0) {
            case LAErrorUserCancel:
            case LAErrorAppCancel:
            case LAErrorSystemCancel:
                *error = tr("cancelled");
                break;
            case LAErrorUserFallback:
                *error = tr("you chose the password instead");
                break;
            default:
                *error = failure ? QString::fromNSString(failure.localizedDescription) : tr("Touch ID failed");
                break;
            }
        }
        return granted;
    }
}

} // namespace

QString displayName()
{
    return QStringLiteral("Touch ID");
}

bool available(QString* why)
{
    return canUse(why);
}

QByteArray unlockKey(bool create, QString* error)
{
    if (!canUse(error) || !askFingerprint(error)) {
        return {};
    }
    const std::shared_ptr<KeyStore> store = keyStore();
    QString problem;
    if (create) {
        QByteArray fresh = PasswordVault::randomBytes(kKeySize);
        if (!store->write(fresh, &problem)) {
            PasswordVault::wipe(fresh);
            if (error) {
                *error = problem;
            }
            return {};
        }
        return fresh;
    }
    const std::optional<QByteArray> key = store->read(&problem);
    if (!key || key->size() != kKeySize) {
        if (error) {
            *error = problem.isEmpty()
                ? tr("ClipItTo's Touch ID key is missing; turn Touch ID off and on again in Settings")
                : problem;
        }
        return {};
    }
    return *key;
}

void removeKey()
{
    keyStore()->remove(nullptr);
}

void cancelPending()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending) {
        [g_pending invalidate];
    }
}

} // namespace clipitto::hello
