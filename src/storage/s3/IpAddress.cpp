#include "IpAddress.h"
#ifdef Q_OS_LINUX
#include <arpa/inet.h>
#include <algorithm>
#else
#include <QHostAddress>
#endif

namespace putgd {
IpAddress classifyIpAddress(const QString& host) {
#ifdef Q_OS_LINUX
    const auto bytes = host.toLatin1();
    in_addr ipv4{};
    if (::inet_pton(AF_INET, bytes.constData(), &ipv4) == 1)
        return {true, (ntohl(ipv4.s_addr) & 0xff000000U) == 0x7f000000U};

    const auto scope = bytes.indexOf('%');
    const auto address = scope < 0 ? bytes : bytes.left(scope);
    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, address.constData(), &ipv6) != 1) return {};
    const auto* p = ipv6.s6_addr;
    const bool mapped = std::all_of(p, p + 10, [](auto value) { return value == 0; }) &&
                        p[10] == 0xff && p[11] == 0xff;
    // Match QHostAddress: mapped IPv4 loopback is accepted, deprecated
    // IPv4-compatible IPv6 addresses (for example ::127.0.0.1) are not.
    return {true, IN6_IS_ADDR_LOOPBACK(&ipv6) || (mapped && p[12] == 127)};
#else
    const QHostAddress address(host);
    return {!address.isNull(), address.isLoopback()};
#endif
}
}
