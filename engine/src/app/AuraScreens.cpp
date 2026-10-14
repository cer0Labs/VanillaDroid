#include "app/AuraScreens.h"

#include "app/GameSession.h"
#include "app/InventoryScreens.h"
#include "app/SocialWindows.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/GameData.h"
#include "game/SpellText.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdlib>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;

} // namespace

void AuraScreens::init(const game::GameData* data, const game::AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_description.reserve(512);
}

void AuraScreens::fill(GameSession& session, u64 guid, View& out, u32 nowMs, bool enchants) {
    out.count = 0;
    game::Gameplay& play = session.play();
    const auto add = [&](const HudAura& aura, const Source& source) {
        if (out.count < out.auras.size()) {
            out.auras[out.count] = aura;
            out.sources[out.count] = source;
            ++out.count;
        }
    };
    // TempEnchant1 (the off hand) and TempEnchant2 (the main hand): the weapon's icon, the enchantment's time
    // left.
    const Entity* me = enchants ? session.entities().find(session.playerGuid()) : nullptr;
    if (me && m_data) {
        std::array<game::WeaponEnchant, kMaxTempEnchants> weapons{};
        const u32 n = game::weaponEnchants(session.entities(), *me, play.enchantTimers(), nowMs, weapons);
        for (u32 i = 0; i < n; ++i) {
            const net::ItemQueryInfo* item = session.item(weapons[i].itemEntry); // asked for when missing
            HudAura a;
            a.iconHash = item ? m_data->itemIconHash(item->displayId) : 0;
            a.remaining = static_cast<f32>(weapons[i].remainingMs) / 1000.0f;
            a.enchant = true;
            Source s;
            s.equipSlot = weapons[i].equipSlot;
            add(a, s);
        }
    }
    const u32 n = play.auras(guid, m_scratch);
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    for (u32 i = 0; i < n; ++i) {
        const game::AuraInfo& info = m_scratch[i];
        const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(info.spellId) : std::nullopt;
        if (!spell || spell->iconHash == 0) {
            continue;
        }
        HudAura a;
        a.iconHash = spell->iconHash;
        a.remaining = info.timed() ? static_cast<f32>(info.remainingMs) / 1000.0f : 0.0f;
        a.stacks = info.stacks;
        a.harmful = !info.positive();
        a.dispel = a.harmful && text ? static_cast<u8>(std::min(text->dispelType(info.spellId), 255u)) : 0;
        add(a, {info.spellId, info.slot, info.casterLevel, info.flags, 0});
    }
}

void AuraScreens::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    VD_PROFILE_ZONE("AuraScreens::prepareHud");
    m_player.count = m_target.count = 0;
    if (!session.inWorld() || !session.entities().find(session.playerGuid())) {
        return;
    }
    fill(session, session.playerGuid(), m_player, nowMs, true);
    hud.playerAuras = {m_player.auras.data(), m_player.count};
    const u64 targetGuid = session.target();
    const Entity* target = targetGuid ? session.entities().find(targetGuid) : nullptr;
    if (target) {
        fill(session, targetGuid, m_target, nowMs, false);
        hud.targetAuras = {m_target.auras.data(), m_target.count};
        // UnitIsFriend("player", "target") puts its buffs first; the target of target (as app/CombatScreens
        // shows it) narrows the debuff rows.
        const Entity* me = session.entities().find(session.playerGuid());
        hud.targetFriendly =
            targetGuid == session.playerGuid() ||
            (me && game::reactionTo(*me, *target, hud.reactions) == game::Reaction::Friendly);
        const u64 totGuid = game::isUnit(*target) ? game::guidValue(*target, uf::UNIT_FIELD_TARGET) : 0;
        const Entity* tot = totGuid ? session.entities().find(totGuid) : nullptr;
        hud.targetOfTarget = tot && game::isUnit(*tot);
    }
}

void AuraScreens::openTip(Owner owner, u64 unit, const Source& source, const ui::Rect& anchor,
                          AuraTooltipAnchor at) {
    if (m_tip.owner == owner && m_tip.unit == unit && m_tip.spellId == source.spellId &&
        m_tip.slot == source.slot) {
        m_tip = Tip{}; // a second tap on the same aura closes it
        return;
    }
    m_tip = Tip{owner, unit, source.spellId, source.slot, anchor, at, {}};
    if (m_trace) {
        VD_LOG_INFO("Aura tooltip: spell %u slot %u of %llx", source.spellId, source.slot,
                    static_cast<unsigned long long>(unit));
    }
}

void AuraScreens::actOnHud(GameSession& session, const HudOutput& out, InventoryScreens& inventory) {
    const i32 index = out.auraTapped >= 0 ? out.auraTapped : out.auraHeld;
    if (index < 0) {
        return;
    }
    const View& view = out.auraOfTarget ? m_target : m_player;
    if (static_cast<u32>(index) >= view.count) {
        return;
    }
    const HudAura& aura = view.auras[static_cast<u32>(index)];
    const Source& source = view.sources[static_cast<u32>(index)];
    if (aura.enchant) { // BuffFrame_EnchantButton_OnEnter: GameTooltip:SetInventoryItem("player", 16 / 17)
        m_tip = Tip{};
        inventory.openItemTooltip({ItemRef::Kind::Slot, 255, static_cast<u8>(source.equipSlot), 0},
                                  out.auraRect);
        return;
    }
    game::Gameplay& play = session.play();
    if (out.auraHeld >= 0 && !out.auraOfTarget) {
        // BuffButton_OnClick (right button): CancelPlayerBuff, for what the server lets go.
        game::AuraInfo info;
        info.spellId = source.spellId;
        info.slot = source.slot;
        info.flags = source.flags;
        game::SpellDetails details;
        if (m_abilities && m_abilities->spellText().details(source.spellId, details) &&
            game::canCancelAura(info, details, play.spells().knows(source.spellId))) {
            play.cancelAura(source.spellId);
            if (m_tip.spellId == source.spellId && m_tip.owner == Owner::Player) {
                m_tip = Tip{};
            }
            if (m_trace) {
                VD_LOG_INFO("Aura: cancel %u (%.*s)", source.spellId, static_cast<int>(details.name.size()),
                            details.name.data());
            }
            return;
        }
    }
    const u64 unit = out.auraOfTarget ? session.target() : session.playerGuid();
    openTip(out.auraOfTarget ? Owner::Target : Owner::Player, unit, source, out.auraRect,
            out.auraOfTarget ? AuraTooltipAnchor::BottomRight : AuraTooltipAnchor::BottomLeft);
    m_listUnit = 0;
}

void AuraScreens::drawPetAuras(GameSession& session, DialogContext& c, const ui::Rect& petFrame, u32 nowMs) {
    m_pet.count = 0;
    const game::PetState& pet = session.play().abilities().pet();
    if (petFrame.w <= 0.0f || !pet.active() || !session.entities().find(pet.guid)) {
        return;
    }
    // RefreshBuffs(PetFrame, 1, "pet"): the pet's first four buffs (UnitBuff), each with the debuff border in
    // the "none" red (the client's showBuffs path never sets a type).
    fill(session, pet.guid, m_pet, nowMs, false);
    std::array<AuraButtonPlace, kMaxPartyDebuffs> places{};
    const f32 k = petFrame.w / 128.0f; // PetFrame is 128 units wide
    u32 count = 0;
    for (u32 i = 0; i < m_pet.count && count < places.size(); ++i) {
        if (m_pet.auras[i].harmful) {
            continue;
        }
        AuraButtonPlace& p = places[count];
        p.aura = i;
        p.icon = {petFrame.x + (48.0f + static_cast<f32>(count) * 17.0f) * k, petFrame.y + 42.0f * k,
                  15.0f * k, 15.0f * k};
        p.hit = p.icon.inset(-1.0f * k);
        ++count;
    }
    const AuraButtonsResult r =
        auraButtons(c.ui, c.draw, c.resources, {m_pet.auras.data(), m_pet.count}, {places.data(), count}, k,
                    nowMs, AuraButtonStyle::Small, "aura.pet", true);
    const i32 index = r.tapped >= 0 ? r.tapped : r.held;
    if (index >= 0) {
        openTip(Owner::Pet, pet.guid, m_pet.sources[static_cast<u32>(index)], r.rect,
                AuraTooltipAnchor::Right);
    }
}

void AuraScreens::partyPetDebuffs(GameSession& session, u64 pet, PartyFrameView& view) {
    // Phase 15 (pets): UnitDebuff("partypetN", 1..4), each bordered in its type's colour.
    const u32 n = session.play().auras(pet, m_scratch);
    view.petDebuffCount = 0;
    for (u32 i = 0; i < n && view.petDebuffCount < view.petDebuffs.size(); ++i) {
        const game::AuraInfo& info = m_scratch[i];
        const std::optional<game::SpellInfo> spell =
            !info.positive() && m_data ? m_data->spell(info.spellId) : std::nullopt;
        if (!spell || spell->iconHash == 0) {
            continue;
        }
        const u32 dispel = m_abilities ? m_abilities->spellText().dispelType(info.spellId) : 0;
        view.petDebuffs[view.petDebuffCount++] = {spell->iconHash, static_cast<u8>(std::min(dispel, 255u))};
    }
}

void AuraScreens::partyDebuffs(GameSession& session, u64 member, PartyFrameView& view, u32 nowMs) {
    // RefreshBuffs(this, 0, "partyN"): UnitDebuff 1..4, each bordered in its type's colour.
    const u32 n = session.play().auras(member, m_scratch);
    u64 signature = 1469598103934665603ull;
    u32 statusDispel = 0;
    view.debuffCount = 0;
    for (u32 i = 0; i < n; ++i) {
        const game::AuraInfo& info = m_scratch[i];
        signature = (signature ^ (static_cast<u64>(info.spellId) << 8 | info.slot)) * 1099511628211ull;
        if (info.positive() || view.debuffCount == view.debuffs.size()) {
            continue;
        }
        const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(info.spellId) : std::nullopt;
        if (!spell || spell->iconHash == 0) {
            continue;
        }
        const u32 dispel = m_abilities ? m_abilities->spellText().dispelType(info.spellId) : 0;
        view.debuffs[view.debuffCount++] = {spell->iconHash, static_cast<u8>(std::min(dispel, 255u))};
        if (hasDebuffType(dispel)) {
            statusDispel = dispel; // the last typed one shown colours the glow
        }
    }
    // The glow (PartyMemberFrame_OnUpdate): a typed debuff, for 30 s after the member's auras last changed.
    PartyGlow* glow = nullptr;
    for (PartyGlow& g : m_glow) {
        if (g.guid == member) {
            glow = &g;
            break;
        }
    }
    if (!glow) { // a new member: the oldest entry
        glow = &m_glow[0];
        for (PartyGlow& g : m_glow) {
            if (g.guid == 0 || static_cast<i32>(g.changedMs - glow->changedMs) < 0) {
                glow = &g;
            }
        }
        *glow = PartyGlow{member, 0, nowMs};
    }
    if (glow->signature != signature) {
        glow->signature = signature;
        glow->changedMs = nowMs;
    }
    const bool shown =
        statusDispel != 0 && static_cast<f32>(nowMs - glow->changedMs) < kPartyStatusSeconds * 1000.0f;
    view.statusColor = shown ? debuffTypeColor(statusDispel) : 0;
    view.statusAlpha = shown ? buffFlashAlpha(nowMs) : 0.0f;
}

void AuraScreens::showPartyAuras(u64 member, const ui::Rect& frame, f32 unit) {
    m_listUnit = member;
    m_listFrame = frame;
    m_listScale = unit;
    m_listPanel = {};
    m_tip = Tip{};
}

void AuraScreens::showAuraTooltip(u64 unit, u32 spellId, u8 slot, const ui::Rect& anchor) {
    Source source;
    source.spellId = spellId;
    source.slot = slot;
    openTip(Owner::Party, unit, source, anchor, AuraTooltipAnchor::Right);
    m_listUnit = 0;
}

bool AuraScreens::tappedElsewhere(const DialogContext& c, const ui::Rect& panel, const ui::Rect& anchor) {
    for (const ui::Touch& t : c.ui.touches()) {
        if (t.released && !t.moved && !t.longPressed && !panel.contains(t.x, t.y) &&
            !anchor.contains(t.x, t.y)) {
            return true;
        }
    }
    return false;
}

void AuraScreens::drawOverlay(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("AuraScreens::drawOverlay");
    if (!session.inWorld()) {
        m_tip = Tip{};
        m_listUnit = 0;
        return;
    }
    // A party member's aura list: its icons show their own tooltips.
    if (m_listUnit != 0) {
        if (tappedElsewhere(c, m_listPanel, m_tip.owner == Owner::Party ? m_tip.panel : ui::Rect{})) {
            m_listUnit = 0;
            m_tip = Tip{};
        } else {
            fill(session, m_listUnit, m_list, nowMs, false);
            const AuraListResult r =
                auraList(c, {{m_list.auras.data(), m_list.count}, m_listFrame, m_listScale}, nowMs);
            m_listPanel = r.panel;
            if (r.panel.w <= 0.0f) {
                m_listUnit = 0; // nothing to list
            } else if (r.tapped >= 0) {
                openTip(Owner::Party, m_listUnit, m_list.sources[static_cast<u32>(r.tapped)], r.rect,
                        AuraTooltipAnchor::Right);
            }
        }
    }
    drawTip(session, c);
}

void AuraScreens::drawTip(GameSession& session, DialogContext& c) {
    if (m_tip.owner == Owner::None) {
        return;
    }
    if (tappedElsewhere(c, m_tip.panel, m_tip.anchor)) {
        m_tip = Tip{};
        return;
    }
    // The aura, still there (it fades, is dispelled, the target changes...).
    const u32 n = session.play().auras(m_tip.unit, m_scratch);
    const game::AuraInfo* aura = nullptr;
    for (u32 i = 0; i < n && !aura; ++i) {
        aura = m_scratch[i].spellId == m_tip.spellId && m_scratch[i].slot == m_tip.slot ? &m_scratch[i]
                                                                                        : nullptr;
    }
    if (!aura || (m_tip.owner == Owner::Target && session.target() != m_tip.unit)) {
        m_tip = Tip{};
        return;
    }
    if (m_textSpell != aura->spellId || m_textLevel != aura->casterLevel) {
        m_textSpell = aura->spellId;
        m_textLevel = aura->casterLevel;
        m_description.clear();
        m_name = m_type = {};
        if (m_abilities) {
            const game::SpellText& text = m_abilities->spellText();
            const Entity* me = session.entities().find(session.playerGuid());
            game::SpellTextContext context;
            context.casterLevel = aura->casterLevel;
            context.gender = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) : 0;
            game::auraDescription(text, aura->spellId, m_description, context);
            m_name = text.name(aura->spellId);
            const u32 dispel = text.dispelType(aura->spellId);
            m_type =
                !aura->positive() && hasDebuffType(dispel) ? text.dispelName(dispel) : std::string_view{};
        }
    }
    AuraTooltipView v;
    v.name = m_name;
    v.type = m_type;
    v.description = m_description;
    if (aura->timed() && aura->remainingMs > 0) {
        v.timeLeft = auraTimeRemaining(m_timeLeft, static_cast<f32>(aura->remainingMs) / 1000.0f);
    }
    v.anchor = m_tip.anchor;
    v.at = m_tip.at;
    const AuraTooltipResult r = auraTooltip(c, v);
    m_tip.panel = r.panel;
    if (r.tapped) {
        m_tip = Tip{}; // a tap on it closes it too
    }
}

} // namespace vd
