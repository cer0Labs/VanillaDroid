#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0005 §5): the PC's IPv4 addresses, to put the right one in the QR. A PC often has several
// (Wi-Fi, Ethernet, VPNs, Hyper-V or VirtualBox adapters); the phone is on the same Wi-Fi or Ethernet
// network, which has a default gateway, a private address and a physical adapter.
namespace vd::setup {

struct LocalAddress {
    u32 address = 0;     // host order
    std::string adapter; // the adapter's name as the system shows it (UTF-8)
    bool wireless = false;
    bool hasGateway = false;
    bool virtualAdapter = false; // VPN, Hyper-V, VirtualBox, VMware, WSL, Docker...

    std::string text() const;
};

// Every IPv4 address of the adapters that are up, but loopback.
std::vector<LocalAddress> localAddresses();

// How likely an address is the phone's network (higher is better).
int lanScore(const LocalAddress& address);
// The likeliest one; nullptr for an empty list.
const LocalAddress* pickLanAddress(std::span<const LocalAddress> addresses);

// Adapter names that are not a real network: "vEthernet (WSL)", "VirtualBox Host-Only", "Tailscale",
// "TAP-Windows".
bool looksVirtual(std::string_view adapterName);

// "192.168.1.20" -> 0xC0A80114; false when it is not a dotted IPv4 address.
bool parseIpv4(std::string_view text, u32& address);

} // namespace vd::setup
