// Phase 17 (honor): the client's own lines of honor and of the insignia (game::NoticeFeed's part), worded as
// WoW.exe words them (game/Honor.h: SMSG_PVP_CREDIT 0x625270, PLAYER_SKINNED in UIParent.lua).
#include "core/Text.h"
#include "game/Gameplay.h"
#include "game/Honor.h"
#include "game/Notices.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

using K = GameEventKind;
namespace uf = net::uf;

bool isPlayer(u64 guid) {
    return guid != 0 && (guid >> 48) == 0;
}

} // namespace

bool NoticeFeed::pvpReady(Gameplay& play, const GameEvent& e) const {
    // A player victim's name not known yet is asked for (the line waits for it, kNameWaitMs at most).
    return e.kind != K::HonorGained || !isPlayer(e.guid) || play.queries().playerName(e.guid) != nullptr;
}

void NoticeFeed::pvpLine(Gameplay& play, const GameEvent& e, const Entity* player, const CombatNames& names) {
    (void)play;
    if (e.kind == K::PlayerSkinned) {
        // UIParent.lua PLAYER_SKINNED: UIErrorsFrame:AddMessage(DEATH_CORPSE_SKINNED, 1.0, 0.1, 0.1) (the
        // SKINNED and SKINNED_REPOP popups are commented out in 1.12.1).
        text::format(push(NoticeKind::Error), "DEATH_CORPSE_SKINNED");
        return;
    }
    // CHAT_MSG_COMBAT_HONOR_GAIN (0x626850 with chat type 0x51), its rank title in the player's gender
    // (0x612BF0).
    const bool female = player && player->byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1;
    std::string_view name = e.guid != 0 ? names.unit(e.guid) : std::string_view{};
    if (name.empty() && e.guid != 0) {
        name = text::get("UNKNOWNOBJECT");
    }
    formatHonorLine(push(NoticeKind::HonorGain), e, name, female);
}

} // namespace vd::game
