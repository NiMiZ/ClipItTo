#include "PhoneNet.h"

#include <QNetworkInterface>
#include <QRandomGenerator>

#include <algorithm>
#include <array>

namespace clipitto::phone {

namespace {

bool localIpv4(quint32 v)
{
    const quint8 a = quint8(v >> 24);
    const quint8 b = quint8(v >> 16);
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

// The IPv4 address of an IPv4 address or of an IPv4-mapped IPv6 one (::ffff:a.b.c.d).
bool ipv4Of(const QHostAddress& address, quint32* out)
{
    if (address.isNull()) {
        return false;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        *out = address.toIPv4Address();
        return true;
    }
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR bytes = address.toIPv6Address();
        for (int i = 0; i < 10; ++i) {
            if (bytes[i] != 0) {
                return false;
            }
        }
        if (bytes[10] != 0xff || bytes[11] != 0xff) {
            return false;
        }
        *out = (quint32(bytes[12]) << 24) | (quint32(bytes[13]) << 16) | (quint32(bytes[14]) << 8) | quint32(bytes[15]);
        return true;
    }
    return false;
}

} // namespace

bool isLocalAddress(const QHostAddress& address)
{
    if (address.isNull()) {
        return false;
    }
    quint32 v4 = 0;
    if (ipv4Of(address, &v4)) {
        return localIpv4(v4);
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol) {
        return false;
    }
    const Q_IPV6ADDR b = address.toIPv6Address();
    bool zeros = true;
    for (int i = 0; i < 15; ++i) {
        zeros = zeros && b[i] == 0;
    }
    if (zeros && b[15] == 1) {
        return true; // ::1
    }
    if ((b[0] & 0xfe) == 0xfc) {
        return true; // fc00::/7
    }
    return b[0] == 0xfe && (b[1] & 0xc0) == 0x80; // fe80::/10
}

bool isLanIpv4(const QHostAddress& address)
{
    quint32 v4 = 0;
    return address.protocol() == QAbstractSocket::IPv4Protocol && ipv4Of(address, &v4) && localIpv4(v4) && (v4 >> 24) != 127;
}

QString newToken()
{
    std::array<quint32, 4> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    QString token;
    for (const quint32 w : words) {
        token += QStringLiteral("%1").arg(w, 8, 16, QLatin1Char('0'));
    }
    words.fill(0);
    return token;
}

bool constantTimeEqual(const QByteArray& a, const QByteArray& b)
{
    const qsizetype n = std::max(a.size(), b.size());
    unsigned diff = unsigned(a.size() != b.size());
    for (qsizetype i = 0; i < n; ++i) {
        const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
        const unsigned y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= x ^ y;
    }
    return diff == 0;
}

QString buildUrl(const QString& ip, quint16 port, const QString& token)
{
    return QStringLiteral("http://%1:%2/%3/").arg(ip).arg(port).arg(token);
}

bool looksVirtual(const InterfaceInfo& info)
{
    if (info.pointToPoint) {
        return true;
    }
    const QString name = info.name.toLower();
    const QString human = info.humanName.toLower();
    // Names that begin like a virtual device (a real "wlan0" or "Wi-Fi" never does).
    static const char* const kPrefixes[] = {"docker", "veth", "virbr", "br-", "vmnet", "vboxnet", "vethernet", "tun", "tap",
                                            "wg", "tailscale", "zt", "utun", "ppp", "lxc", "lxd", "podman", "cni", "flannel",
                                            "awdl", "llw", "anpi", "isatap", "teredo", "bridge"};
    // Words that say it anywhere in a name ("VMware Network Adapter VMnet8", "vEthernet (WSL)").
    static const char* const kWords[] = {"virtual", "vmware", "virtualbox", "hyper-v", "vethernet", "wsl", "vpn", "docker",
                                         "tailscale", "zerotier", "loopback", "pseudo"};
    const auto startsWith = [](const QString& s, const char* p) { return s.startsWith(QLatin1String(p)); };
    const auto contains = [](const QString& s, const char* p) { return s.contains(QLatin1String(p)); };
    return std::any_of(std::begin(kPrefixes), std::end(kPrefixes),
                       [&](const char* p) { return startsWith(name, p) || startsWith(human, p); })
        || std::any_of(std::begin(kWords), std::end(kWords), [&](const char* w) { return contains(name, w) || contains(human, w); });
}

QList<Candidate> lanCandidates(const QList<InterfaceInfo>& interfaces)
{
    struct Ranked {
        Candidate candidate;
        int rank = 0;
        int order = 0;
    };
    QList<Ranked> ranked;
    int order = 0;
    for (const InterfaceInfo& info : interfaces) {
        if (!info.up || info.loopback) {
            continue;
        }
        const bool virt = looksVirtual(info);
        for (const QHostAddress& address : info.addresses) {
            if (!isLanIpv4(address)) {
                continue;
            }
            const quint32 v4 = address.toIPv4Address();
            const quint8 a = quint8(v4 >> 24);
            const quint8 b = quint8(v4 >> 16);
            const int range = (a == 192 && b == 168) ? 0 : a == 10 ? 1 : a == 172 ? 2 : 3;
            Ranked r;
            r.candidate = {address.toString(), info.humanName.isEmpty() ? info.name : info.humanName, virt};
            r.rank = (virt ? 10 : 0) + range;
            r.order = order++;
            ranked.append(r);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& x, const Ranked& y) {
        return x.rank != y.rank ? x.rank < y.rank : x.order < y.order;
    });
    QList<Candidate> out;
    for (const Ranked& r : ranked) {
        out.append(r.candidate);
    }
    return out;
}

QList<InterfaceInfo> systemInterfaces()
{
    QList<InterfaceInfo> out;
    const auto all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : all) {
        InterfaceInfo info;
        info.name = iface.name();
        info.humanName = iface.humanReadableName();
        const auto flags = iface.flags();
        info.up = flags.testFlag(QNetworkInterface::IsUp) && flags.testFlag(QNetworkInterface::IsRunning);
        info.loopback = flags.testFlag(QNetworkInterface::IsLoopBack);
        info.pointToPoint = flags.testFlag(QNetworkInterface::IsPointToPoint);
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            info.addresses.append(entry.ip());
        }
        out.append(info);
    }
    return out;
}

} // namespace clipitto::phone
