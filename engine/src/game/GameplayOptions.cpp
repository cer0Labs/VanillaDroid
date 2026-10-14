// Phase 15 (options): what the server keeps of the interface options (the same class as Gameplay.cpp): the helm and the
// cloak shown, the extra action bars. vmangos-core 4641790: WorldSession::HandleShowingHelmOpcode / _CloakOpcode toggle
// PLAYER_FLAGS_HIDE_HELM (0x400) / _HIDE_CLOAK (0x800); HandleSetActionBarTogglesOpcode stores the byte in
// PLAYER_FIELD_BYTES byte 2 (PLAYER_FIELD_BYTES_OFFSET_ACTION_BARS), saved with the character. WoW.exe: ShowingHelm
// 0x48DD60 reads the flag, ShowHelm 0x48DE20 sends the toggle only when the wish differs; SetActionBarToggles 0x4E76E0
// packs its first four arguments as bits 0..3 (ALWAYS_SHOW_MULTIBARS, the fifth, stays on the client).

#include "game/Gameplay.h"

#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

constexpr u32 kPlayerFlagHideHelm = 0x400;  // PLAYER_FLAGS_HIDE_HELM
constexpr u32 kPlayerFlagHideCloak = 0x800; // PLAYER_FLAGS_HIDE_CLOAK
constexpr u8 kActionBarsByte = 2;           // PLAYER_FIELD_BYTES_OFFSET_ACTION_BARS

} // namespace

bool Gameplay::showingHelm() const {
    const Entity* player = me();
    return !player || (player->value(net::uf::PLAYER_FLAGS) & kPlayerFlagHideHelm) == 0;
}

bool Gameplay::showingCloak() const {
    const Entity* player = me();
    return !player || (player->value(net::uf::PLAYER_FLAGS) & kPlayerFlagHideCloak) == 0;
}

void Gameplay::showHelm(bool show) {
    if (m_inWorld && me() && show != showingHelm()) {
        send(net::Opcode::CMSG_TOGGLE_HELM);
    }
}

void Gameplay::showCloak(bool show) {
    if (m_inWorld && me() && show != showingCloak()) {
        send(net::Opcode::CMSG_TOGGLE_CLOAK);
    }
}

u8 Gameplay::actionBarToggles() const {
    const Entity* player = me();
    return player ? static_cast<u8>(player->byteValue(net::uf::PLAYER_FIELD_BYTES, kActionBarsByte) & 0x0F) : 0;
}

void Gameplay::setActionBarToggles(u8 bars) {
    if (!m_inWorld || bars == actionBarToggles()) {
        return;
    }
    m_body.clear();
    m_body.push_back(static_cast<u8>(bars & 0x0F));
    sendBody(net::Opcode::CMSG_SET_ACTIONBAR_TOGGLES);
}

} // namespace vd::game
