#pragma once

#include "core/Types.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Where a SpellVisualKit puts its effect models on a unit (SpellVisualKit.dbc fields 3..12, in this order).
enum class KitSlot : u8 {
    Head,
    Chest,
    Base,
    LeftHand,
    RightHand,
    Breath,
    LeftWeapon,
    RightWeapon,
    Special,
    World, // in the world where the unit stands, not following it
    Count
};
constexpr u32 kKitSlots = static_cast<u32>(KitSlot::Count);

// The M2 attachment point a kit slot goes to on a unit's model (every creature and race model has 15-22 and
// 34: the spell points), then a fallback when the model lacks it.
u32 kitAttachment(KitSlot slot);
u32 kitFallbackAttachment(KitSlot slot);

constexpr u16 kNoKitAnimation = 0xFFFF;

// A SpellVisualKit.dbc row: what plays on a unit at a stage of a spell (precast, cast, impact, state,
// channel) or for SMSG_PLAY_SPELL_VISUAL / _IMPACT. Its sound is the audio code's (audio::SoundData).
struct VisualKit {
    u32 id = 0;
    u32 type = 0;                    // field 1 (0 casts, 1 impacts, 2 states and channels, 3 areas)
    u16 animation = kNoKitAnimation; // AnimationData id the unit plays, kNoKitAnimation: none
    std::array<u64, kKitSlots>
        models{};       // actors.vdpk hash of each slot's model (Effects::effectModelHash), 0 = none
    u32 modelCount = 0; // slots with a model
};

// A SpellVisual.dbc row (vMaNGOS SpellVisualEntry): a spell's kits by stage, its missile and its area effect.
struct SpellVisualInfo {
    u32 id = 0;
    u32 precastKit = 0;
    u32 castKit = 0;
    u32 impactKit = 0;
    u32 stateKit = 0;
    u32 channelKit = 0;
    u64 missileModel = 0;      // flies from the caster to each target (0: none)
    u32 missilePath = 0;       // field 8: 0 straight, 2 arched
    u32 missileAttachment = 0; // field 9: where it lands on the target (1 chest, 0 base, 2 head...)
    u64 areaModel = 0;         // a ground spell's area (Blizzard, Flamestrike, Consecration), 0: none
    u32 areaKit = 0;
};

// The spell visual tables of the client (phase 9): Spell.dbc (visual and missile speed), SpellVisual,
// SpellVisualKit, SpellVisualEffectName (the models, as actors.vdpk hashes), and the emote animations
// (Emotes, EmotesText). Field numbers of the 1.12.1 files (vMaNGOS DBCStructure.h for SpellVisual and Emotes;
// the kit columns from the client's own data), checked against the real tables in
// tests/game/test_spell_visual_data.cpp. Read-only after init(): lookups never allocate.
class SpellVisualData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_kits.empty(); }

    const VisualKit* kit(u32 id) const;
    const SpellVisualInfo* visual(u32 id) const;
    // A spell's visual (nullptr: none) and missile speed (yards per second; 0: no missile, it hits at once).
    const SpellVisualInfo* spellVisual(u32 spellId) const;
    f32 missileSpeed(u32 spellId) const;
    // A shot with a ranged weapon (Auto Shot, Shoot, Throw, the NPCs' Shoot Bow / Gun): the weapon subclasses
    // it needs (Spell.dbc EquippedItemSubClassMask of a weapon: bow 0x4, gun 0x8, thrown 0x10000, crossbow
    // 0x40000, wand 0x80000); 0 for anything else.
    u32 rangedWeaponMask(u32 spellId) const;
    // Development and tests: a model's client path by its hash ("Spells\Frostbolt.mdx"), empty if unknown.
    std::string_view modelPath(u64 model) const;

    // Enchantments (SpellItemEnchantment.dbc m_itemVisual -> ItemVisuals.dbc -> ItemVisualEffects.dbc): the
    // glow models along a weapon, one per weapon attachment point 0-4 (0: none there). False when it shows
    // nothing.
    static constexpr u32 kItemVisualSlots = 5;
    bool enchantModels(u32 enchantmentId, std::array<u64, kItemVisualSlots>& out) const;

    // Emotes.dbc: the animation of an emote (kNoKitAnimation: none) and whether it is a one-shot (EmoteType
    // 0; states loop through UNIT_NPC_EMOTESTATE instead). EmotesText.dbc: the emote of a text emote (/wave).
    u16 emoteAnimation(u32 emoteId) const;
    bool emoteOneShot(u32 emoteId) const;
    u32 textEmote(u32 emotesTextId) const;

    // Tests: tables built by hand.
    void addKit(const VisualKit& kit);
    void addVisual(const SpellVisualInfo& visual);
    void addSpell(u32 spellId, u32 visualId, f32 missileSpeed, u32 rangedMask = 0);

    static constexpr u32 kRangedWeapons = 0x4 | 0x8 | 0x10000 | 0x40000 | 0x80000;

private:
    struct SpellRow {
        u32 id = 0;
        u32 visual = 0;
        f32 speed = 0.0f;
        u32 rangedMask = 0;
    };
    const SpellRow* spell(u32 spellId) const;
    struct Emote {
        u16 animation = kNoKitAnimation;
        bool oneShot = true;
    };

    std::vector<VisualKit> m_kits;
    std::unordered_map<u32, u32> m_kitIndex;
    std::vector<SpellVisualInfo> m_visuals;
    std::unordered_map<u32, u32> m_visualIndex;
    std::vector<SpellRow> m_spells; // sorted by id (binary search: ~22k rows without a node each)
    std::unordered_map<u64, std::string_view> m_modelPaths;
    std::unordered_map<u32, Emote> m_emotes;
    std::unordered_map<u32, u32> m_textEmotes;
    std::unordered_map<u32, std::array<u64, kItemVisualSlots>> m_enchantModels; // enchantment -> models
};

} // namespace vd::game
