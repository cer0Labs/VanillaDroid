#pragma once

#include "app/AuraLayout.h"
#include "app/AuraWindows.h"
#include "app/Dialogs.h"
#include "app/Hud.h"
#include "core/Types.h"
#include "game/Auras.h"
#include "game/WeaponEnchants.h"

#include <array>
#include <string>
#include <string_view>

namespace vd {

class GameSession;
class InventoryScreens;
struct PartyFrameView;
namespace game {
class AbilityData;
class GameData;
} // namespace game

// Parity (auras): the game's side of the aura buttons. Fills the HUD's views from game::Gameplay — the
// player's buffs, debuffs and weapon enchantments (BuffFrame), the target's auras and how TargetFrame orders
// them, the party frames' debuffs and portrait glow (PartyMemberFrame), the pet frame's buffs (PetFrame) —
// and applies what the player does with them: a tap shows the aura's tooltip (GameTooltip:SetPlayerBuff /
// SetUnitBuff / SetUnitDebuff; a weapon enchantment its weapon's: SetInventoryItem), a long press on one of
// the player's buffs cancels it as the client's right click does (CancelPlayerBuff: only what the server lets
// go, game::canCancelAura), a long press on a party frame lists the member's auras (PartyMemberBuffTooltip).
// Main thread; fixed storage: nothing allocates per frame once warmed up (a tooltip's text is resolved when
// it opens or its aura changes).
class AuraScreens {
public:
    // `abilities` (Spell.dbc's texts and dispel types) may be null: no tooltips' texts, red debuff borders.
    void init(const game::GameData* data, const game::AbilityData* abilities);

    // Before the HUD draws: its player and target aura views (HudFrame::playerAuras, targetAuras,
    // targetFriendly, targetOfTarget).
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);
    // After the HUD drew: its aura taps and long presses.
    void actOnHud(GameSession& session, const HudOutput& out, InventoryScreens& inventory);
    // The pet frame's four buffs (PetFrame.xml: PetFrameDebuff1..4 at (48, 42), RefreshBuffs(PetFrame, 1,
    // "pet")) on the frame app/CombatScreens drew at `petFrame` (empty: no pet).
    void drawPetAuras(GameSession& session, DialogContext& c, const ui::Rect& petFrame, u32 nowMs);
    // A party frame's view: its first four debuffs and the portrait's glow (RefreshBuffs(this, 0, "partyN")).
    void partyDebuffs(GameSession& session, u64 member, PartyFrameView& view, u32 nowMs);
    // Phase 15 (pets): the party pet frame's four debuffs (RefreshBuffs(PartyMemberFrameNPetFrame, 0,
    // "partypetN"); the pet frame has no glow).
    void partyPetDebuffs(GameSession& session, u64 pet, PartyFrameView& view);
    // A long press on a party frame (`frame`, drawn `unit` logical pixels per interface unit): the member's
    // aura list.
    void showPartyAuras(u64 member, const ui::Rect& frame, f32 unit);
    // Phase 16 (pullouts): a raid member's aura tapped on its pullout (RaidBuffButtonTemplate's OnEnter:
    // GameTooltip:SetUnitBuff / SetUnitDebuff) beside the button `anchor`; a second tap closes it.
    void showAuraTooltip(u64 unit, u32 spellId, u8 slot, const ui::Rect& anchor);
    // Last, over the HUD and the windows: the aura list and the tooltip that are open.
    void drawOverlay(GameSession& session, DialogContext& c, u32 nowMs);
    // The tooltip open (development, tests): the spell, 0 none.
    u32 tooltipSpell() const { return m_tip.spellId; }

private:
    enum class Owner : u8 { None, Player, Target, Pet, Party };
    // Where a view's aura came from.
    struct Source {
        u32 spellId = 0;
        u8 slot = 0;
        u8 casterLevel = 0;
        u8 flags = 0;
        u32 equipSlot = 0; // an enchantment: its weapon's slot (game::kEquipMainHand / kEquipOffHand)
    };
    struct View {
        std::array<HudAura, game::kMaxAuras + kMaxTempEnchants> auras{};
        std::array<Source, game::kMaxAuras + kMaxTempEnchants> sources{};
        u32 count = 0;
    };
    struct Tip {
        Owner owner = Owner::None;
        u64 unit = 0;
        u32 spellId = 0;
        u8 slot = 0;
        ui::Rect anchor;
        AuraTooltipAnchor at = AuraTooltipAnchor::BottomLeft;
        ui::Rect panel; // drawn last frame
    };
    // The party portraits' glow: when each member's auras last changed (RefreshBuffs resets a 30 s
    // countdown).
    struct PartyGlow {
        u64 guid = 0;
        u64 signature = 0;
        u32 changedMs = 0;
    };

    // A unit's auras as views (the player's with their time left), in slot order; `enchants` first (the
    // player's).
    void fill(GameSession& session, u64 guid, View& out, u32 nowMs, bool enchants);
    void openTip(Owner owner, u64 unit, const Source& source, const ui::Rect& anchor, AuraTooltipAnchor at);
    // True while it should stay open: a tap that lands on neither `panel` nor `anchor` closes it.
    static bool tappedElsewhere(const DialogContext& c, const ui::Rect& panel, const ui::Rect& anchor);
    void drawTip(GameSession& session, DialogContext& c);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    bool m_trace = false; // VD_TRACE_GAMEPLAY

    std::array<game::AuraInfo, game::kMaxAuras> m_scratch{};
    View m_player, m_target, m_pet, m_list;
    std::array<PartyGlow, 4> m_glow{};

    Tip m_tip;
    // The tooltip's texts, resolved when its aura changes.
    u32 m_textSpell = 0;
    u8 m_textLevel = 0;
    std::string m_description;
    std::string_view m_name, m_type;
    std::array<char, 64> m_timeLeft{};

    // The party member whose aura list is open (0: none), beside its frame.
    u64 m_listUnit = 0;
    ui::Rect m_listFrame;
    f32 m_listScale = 1.0f;
    ui::Rect m_listPanel;
};

} // namespace vd
