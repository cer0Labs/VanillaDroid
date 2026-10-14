#include "NetInterfaces.h"

#include "HttpServer.h"
#include "SetupCommon.h"

#include <algorithm>
#include <array>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h> // after winsock2.h
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace vd::setup {
namespace {

bool isPrivate(u32 address) {
    const u32 a = address >> 24;
    const u32 b = (address >> 16) & 255;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

#if defined(_WIN32)
std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), length, nullptr, nullptr);
    }
    return out;
}
#endif

} // namespace

std::string LocalAddress::text() const {
    return ipv4Text(address);
}

bool looksVirtual(std::string_view adapterName) {
    static constexpr std::array<std::string_view, 16> kWords = {
        "virtual", "vethernet", "hyper-v",   "vmware",   "vbox",      "tap-",    "tun",      "vpn",
        "wsl",     "docker",    "tailscale", "zerotier", "wireguard", "hamachi", "loopback", "utun"};
    const std::string name = toLowerAscii(adapterName);
    return std::any_of(kWords.begin(), kWords.end(),
                       [&](std::string_view word) { return name.find(word) != std::string::npos; });
}

bool parseIpv4(std::string_view text, u32& address) {
    u32 parts[4] = {};
    u32 part = 0;
    usize digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            parts[part] = parts[part] * 10 + static_cast<u32>(c - '0');
            if (++digits > 3 || parts[part] > 255) {
                return false;
            }
        } else if (c == '.' && digits > 0 && part < 3) {
            ++part;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 3 || digits == 0) {
        return false;
    }
    address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

int lanScore(const LocalAddress& address) {
    int score = 0;
    if (isPrivate(address.address)) {
        score += 8;
    }
    if (address.hasGateway) {
        score += 4;
    }
    if (!address.virtualAdapter) {
        score += 16;
    }
    if (address.wireless) {
        score += 1; // the phone is on Wi-Fi: a PC on both is most likely reached there
    }
    if ((address.address >> 16) == 0xA9FE) { // 169.254: no DHCP answered
        score -= 32;
    }
    return score;
}

const LocalAddress* pickLanAddress(std::span<const LocalAddress> addresses) {
    const LocalAddress* best = nullptr;
    for (const LocalAddress& address : addresses) {
        if (!best || lanScore(address) > lanScore(*best)) {
            best = &address;
        }
    }
    return best;
}

std::vector<LocalAddress> localAddresses() {
    std::vector<LocalAddress> list;
#if defined(_WIN32)
    ULONG size = 32 * 1024;
    std::vector<u8> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return list;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        const std::string name = narrow(adapter->FriendlyName);
        const std::string description = narrow(adapter->Description);
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const sockaddr* raw = unicast->Address.lpSockaddr;
            if (!raw || raw->sa_family != AF_INET) {
                continue;
            }
            LocalAddress address;
            address.address = ntohl(reinterpret_cast<const sockaddr_in*>(raw)->sin_addr.s_addr);
            address.adapter = name;
            address.wireless = adapter->IfType == IF_TYPE_IEEE80211;
            address.hasGateway = adapter->FirstGatewayAddress != nullptr;
            address.virtualAdapter = looksVirtual(name) || looksVirtual(description) ||
                                     adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP;
            list.push_back(std::move(address));
        }
    }
#else
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        return list;
    }
    for (ifaddrs* entry = addresses; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !(entry->ifa_flags & IFF_UP) ||
            (entry->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        LocalAddress address;
        address.address = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
        address.adapter = entry->ifa_name ? entry->ifa_name : "";
        // No portable gateway query: the usual names of the physical adapters stand in for it.
        const std::string name = toLowerAscii(address.adapter);
        const bool physical =
            name.rfind("en", 0) == 0 || name.rfind("eth", 0) == 0 || name.rfind("wl", 0) == 0;
        address.wireless = name.rfind("wl", 0) == 0 || name == "en0";
        address.hasGateway = physical;
        address.virtualAdapter = looksVirtual(address.adapter) || (entry->ifa_flags & IFF_POINTOPOINT) != 0;
        list.push_back(std::move(address));
    }
    freeifaddrs(addresses);
#endif
    if (list.empty()) {
        return list;
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const LocalAddress& a, const LocalAddress& b) { return lanScore(a) > lanScore(b); });
    return list;
}

} // namespace vd::setup
