#include "game/Progress.h"

#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {

namespace uf = net::uf;

u32 talentRank(const TalentInfo& talent, const SpellBook& spells) {
    for (u32 k = kMaxTalentRanks; k-- > 0;) {
        if (talent.ranks[k] != 0 && spells.knows(talent.ranks[k])) {
            return k + 1;
        }
    }
    return 0;
}

u32 treePoints(const AbilityData& data, u32 tabId, const SpellBook& spells) {
    u32 points = 0;
    for (const TalentInfo& t : data.talents(tabId)) {
        points += talentRank(t, spells);
    }
    return points;
}

TalentStatus talentStatus(const AbilityData& data, const TalentInfo& talent, const SpellBook& spells,
                          u32 freePoints) {
    TalentStatus s;
    s.rank = talentRank(talent, spells);
    s.maxRank = talent.rankCount();
    s.rowOpen = treePoints(data, talent.tab, spells) >= talent.row * kTalentPointsPerRow;
    if (talent.dependsOn != 0) {
        const TalentInfo* other = data.talent(talent.dependsOn);
        s.prerequisiteMet = other != nullptr && talentRank(*other, spells) > talent.dependsOnRank;
    }
    if (talent.dependsOnSpell != 0 && !spells.knows(talent.dependsOnSpell)) {
        s.prerequisiteMet = false;
    }
    s.available = freePoints > 0 && s.rank < s.maxRank && s.rowOpen && s.prerequisiteMet;
    return s;
}

u32 readSkills(const Entity& player, std::span<SkillValue> out) {
    u32 count = 0;
    for (u32 i = 0; i < kMaxSkills && count < out.size(); ++i) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + i * 3);
        const u32 idStep = player.value(base);
        const u32 id = idStep & 0xFFFFu;
        if (id == 0) {
            continue;
        }
        const u32 values = player.value(static_cast<u16>(base + 1));
        const u32 bonuses = player.value(static_cast<u16>(base + 2));
        SkillValue& s = out[count++];
        s.id = id;
        s.step = idStep >> 16;
        s.value = values & 0xFFFFu;
        s.max = values >> 16;
        s.bonus = static_cast<i16>(bonuses & 0xFFFFu) + static_cast<i16>(bonuses >> 16);
    }
    return count;
}

u32 comboPoints(const Entity& player, u64 target) {
    if (target == 0 || guidValue(player, uf::PLAYER_FIELD_COMBO_TARGET) != target) {
        return 0;
    }
    return player.byteValue(uf::PLAYER_FIELD_BYTES, 1);
}

} // namespace vd::game
