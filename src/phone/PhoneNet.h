#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QString>

// Pure helpers for "Receive from phone": which addresses count as the local network, the token, the address in the QR code.
// No sockets are opened here.
namespace clipitto::phone {

// Whether `address` is on the local network of this computer or its own: loopback, 10/8, 172.16/12, 192.168/16, link-local
// 169.254/16, and for IPv6 ::1, fc00::/7 and fe80::/10. An IPv4-mapped IPv6 address (::ffff:a.b.c.d) is judged as the IPv4
// address it carries. Everything else (public addresses, 0.0.0.0, multicast, a null address) is not local.
bool isLocalAddress(const QHostAddress& address);

// A private network address a phone on the same Wi-Fi could reach (10/8, 172.16/12, 192.168/16, 169.254/16), not loopback.
bool isLanIpv4(const QHostAddress& address);

// 128 random bits from the system's generator, as 32 lowercase hex digits.
QString newToken();

// Compares two byte strings in time that depends only on the length of the longer one (not on where they differ).
bool constantTimeEqual(const QByteArray& a, const QByteArray& b);

// "http://<ip>:<port>/<token>/". The address is an IPv4 text.
QString buildUrl(const QString& ip, quint16 port, const QString& token);

// One network interface as the system reports it (a plain struct, so the choice below can be tested with a table).
struct InterfaceInfo {
    QString name; // "wlan0", "eth0", "vEthernet (WSL)"
    QString humanName; // what the system shows the user
    bool up = true; // up and running
    bool loopback = false;
    bool pointToPoint = false; // tunnels and VPNs usually are
    QList<QHostAddress> addresses;
};

struct Candidate {
    QString address; // IPv4 text
    QString interfaceName; // humanName if there is one, else name
    bool likelyVirtual = false; // docker, VM, VPN, WSL...: listed last
};

// Whether the interface looks virtual (docker, veth, bridge, VMware, VirtualBox, Hyper-V vEthernet, WSL, VPN, tunnel...).
bool looksVirtual(const InterfaceInfo& info);

// The addresses a phone could use, the best first: real Wi-Fi/Ethernet interfaces before virtual ones, then 192.168 before
// 10 before 172.16 before link-local. Interfaces that are down, loopback or without a private IPv4 address give none.
QList<Candidate> lanCandidates(const QList<InterfaceInfo>& interfaces);

// This computer's interfaces (QNetworkInterface).
QList<InterfaceInfo> systemInterfaces();

} // namespace clipitto::phone
