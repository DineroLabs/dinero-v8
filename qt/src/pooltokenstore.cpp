// Copyright (c) 2026 Dinero Labs.

#include "pooltokenstore.h"

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace {

class NoTokenStore final : public PoolTokenStore {
public:
    bool available() const override { return false; }
    QString load(const QString&) const override { return {}; }
    bool save(const QString&, const QString&) override { return false; }
    void remove(const QString&) override {}
};

#ifdef Q_OS_MACOS
// A generic-password item per (service, account): the account is the pool's
// status endpoint, so each remembered pool keeps its own token.
class KeychainTokenStore final : public PoolTokenStore {
public:
    explicit KeychainTokenStore(QString service) : service_(std::move(service)) {}

    bool available() const override { return true; }

    QString load(const QString& account) const override {
        CFMutableDictionaryRef query = baseQuery(account);
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);
        if (status != errSecSuccess || !result) return {};
        const auto data = static_cast<CFDataRef>(result);
        const QString token = QString::fromUtf8(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                                                static_cast<qsizetype>(CFDataGetLength(data)));
        CFRelease(result);
        return token;
    }

    bool save(const QString& account, const QString& token) override {
        remove(account);  // replace rather than collide with an older item
        const QByteArray bytes = token.toUtf8();
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.constData()),
                                      bytes.size());
        CFMutableDictionaryRef item = baseQuery(account);
        CFDictionarySetValue(item, kSecValueData, data);
        CFDictionarySetValue(item, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);
        CFStringRef label = QStringLiteral("Dinero pool ops token").toCFString();
        CFDictionarySetValue(item, kSecAttrLabel, label);
        const OSStatus status = SecItemAdd(item, nullptr);
        CFRelease(label);
        CFRelease(item);
        CFRelease(data);
        return status == errSecSuccess;
    }

    void remove(const QString& account) override {
        CFMutableDictionaryRef query = baseQuery(account);
        SecItemDelete(query);
        CFRelease(query);
    }

private:
    CFMutableDictionaryRef baseQuery(const QString& account) const {
        CFMutableDictionaryRef query = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef service = service_.toCFString();
        CFStringRef acct = account.toCFString();
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service);
        CFDictionarySetValue(query, kSecAttrAccount, acct);
        CFRelease(service);
        CFRelease(acct);
        return query;
    }

    QString service_;
};
#endif

}  // namespace

std::unique_ptr<PoolTokenStore> makeSystemPoolTokenStore(const QString& service) {
#ifdef Q_OS_MACOS
    return std::make_unique<KeychainTokenStore>(service);
#else
    Q_UNUSED(service);
    return std::make_unique<NoTokenStore>();
#endif
}
