// A ClipItTo key in the macOS Keychain (a generic password item in the user's login keychain,
// service io.github.nimiz.ClipItTo, one account per entry). Security.framework only.
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "SystemKeyStore.h"

namespace clipitto {

namespace {

NSString* const kService = @"io.github.nimiz.ClipItTo";

QString describe(OSStatus status, const char* what)
{
    CFStringRef text = SecCopyErrorMessageString(status, nullptr);
    const QString message = text ? QString::fromCFString(text) : QStringLiteral("error %1").arg(int(status));
    if (text) {
        CFRelease(text);
    }
    return QStringLiteral("%1: %2").arg(QLatin1String(what), message);
}

class MacKeyStore : public KeyStore {
public:
    explicit MacKeyStore(const QString& entry) : m_account(entry.toNSString()) {}

    bool available(QString*) override { return true; } // the Keychain is part of macOS

    std::optional<QByteArray> read(QString* error) override
    {
        @autoreleasepool {
            NSDictionary* lookup = query(@{
                (__bridge id)kSecReturnData : @YES,
                (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne
            });
            CFTypeRef found = nullptr;
            const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)lookup, &found);
            if (status == errSecItemNotFound) {
                if (error) {
                    error->clear();
                }
                return std::nullopt;
            }
            if (status != errSecSuccess) {
                if (error) {
                    *error = describe(status, "SecItemCopyMatching");
                }
                return std::nullopt;
            }
            NSData* data = CFBridgingRelease(found);
            return QByteArray(static_cast<const char*>(data.bytes), qsizetype(data.length));
        }
    }

    bool write(const QByteArray& key, QString* error) override
    {
        @autoreleasepool {
            NSData* data = [NSData dataWithBytes:key.constData() length:NSUInteger(key.size())];
            OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query(@{}),
                                            (__bridge CFDictionaryRef) @{(__bridge id)kSecValueData : data});
            if (status == errSecItemNotFound) {
                status = SecItemAdd((__bridge CFDictionaryRef)query(@{(__bridge id)kSecValueData : data}), nullptr);
            }
            if (status != errSecSuccess) {
                if (error) {
                    *error = describe(status, "SecItemAdd");
                }
                return false;
            }
            return true;
        }
    }

    bool remove(QString* error) override
    {
        @autoreleasepool {
            const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query(@{}));
            if (status != errSecSuccess && status != errSecItemNotFound) {
                if (error) {
                    *error = describe(status, "SecItemDelete");
                }
                return false;
            }
            return true;
        }
    }

private:
    // The item's identity (class, service, account) plus `extra`.
    NSDictionary* query(NSDictionary* extra) const
    {
        NSMutableDictionary* q = [NSMutableDictionary dictionaryWithDictionary:@{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : kService,
            (__bridge id)kSecAttrAccount : m_account
        }];
        [q addEntriesFromDictionary:extra];
        return q;
    }

    NSString* m_account;
};

} // namespace

std::shared_ptr<KeyStore> createSystemKeyStore(const QString& entry)
{
    return std::make_shared<MacKeyStore>(entry);
}

} // namespace clipitto
