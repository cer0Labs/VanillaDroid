#include "game/SpellVisualData.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "fx/EffectSet.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <string>

namespace vd::game {
namespace {

// Field numbers of the 1.12.1 (5875) files.
// Spell.dbc (see GameData, SoundData): speed, visual, and the weapon a shot needs (EquippedItemClass, its
// subclass mask; vMaNGOS SpellEntry, the same numbers up to the effects).
constexpr u32 kSpellSpeed = 37, kSpellVisual = 115, kSpellItemClass = 58, kSpellItemSubclasses = 59,
              kSpellFields = 173;
constexpr u32 kItemClassWeapon = 2;
// SpellVisual.dbc (16 fields, vMaNGOS SpellVisualEntry).
constexpr u32 kVisualPrecast = 1, kVisualCast = 2, kVisualImpact = 3, kVisualState = 4, kVisualChannel = 5,
              kVisualHasMissile = 6, kVisualMissileModel = 7, kVisualMissilePath = 8,
              kVisualMissileAttachment = 9, kVisualHasArea = 11, kVisualAreaModel = 12, kVisualAreaKit = 13,
              kVisualFields = 16;
// SpellVisualKit.dbc (35 fields): id, kit type, animation, 10 effect slots (KitSlot order), sound, camera
// shake, 4 character procs and their 16 parameters.
constexpr u32 kKitType = 1, kKitAnimation = 2, kKitFirstSlot = 3, kKitFields = 35;
// SpellVisualEffectName.dbc: id, name, model path.
constexpr u32 kEffectPath = 2, kEffectFields = 5;
// Emotes.dbc: id, name, animation, flags, EmoteType (0 one-shot, 1-2 states), stand state, sound.
constexpr u32 kEmoteAnimation = 2, kEmoteType = 4, kEmoteFields = 7;
constexpr u32 kTextEmoteEmote = 2, kTextEmoteFields = 19; // EmotesText.dbc: id, name, Emotes id, texts...
// SpellItemEnchantment.dbc (vMaNGOS SpellItemEnchantmentEntry: field 22 m_itemVisual), ItemVisuals.dbc (id,
// the ItemVisualEffects of weapon points 0-4), ItemVisualEffects.dbc (id, model path).
constexpr u32 kEnchantItemVisual = 22, kEnchantFields = 24;
constexpr u32 kItemVisualFirst = 1, kItemVisualFields = 6;
constexpr u32 kItemEffectPath = 1, kItemEffectFields = 2;

bool open(const pack::PackReader& db, const char* name, u32 fields, db::DbcView& view) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing: no spell visuals from it", name);
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < fields) {
        VD_LOG_WARN("%s.dbc: unexpected layout, no spell visuals from it", name);
        return false;
    }
    view = parsed.value();
    return true;
}

} // namespace

u32 kitAttachment(KitSlot slot) {
    switch (slot) {
    case KitSlot::Head: return 20;      // Head: over it (every model has it; helms use 11)
    case KitSlot::Chest: return 34;     // Chest
    case KitSlot::Base: return 19;      // Base: at the feet
    case KitSlot::LeftHand: return 21;  // SpellLeftHand
    case KitSlot::RightHand: return 22; // SpellRightHand
    case KitSlot::Breath: return 17;    // Breath
    case KitSlot::LeftWeapon: return 2; // the weapon sits in the hand
    case KitSlot::RightWeapon: return 1;
    case KitSlot::Special: return 23; // Special1
    case KitSlot::World: return 19;
    default: return 19;
    }
}

u32 kitFallbackAttachment(KitSlot slot) {
    switch (slot) {
    case KitSlot::Head: return 11;
    case KitSlot::Chest: return 15; // ChestBloodFront
    case KitSlot::LeftHand: return 2;
    case KitSlot::RightHand: return 1;
    case KitSlot::Breath: return 20;
    case KitSlot::LeftWeapon: return 21;
    case KitSlot::RightWeapon: return 22;
    case KitSlot::Special: return 34;
    default: return 19;
    }
}

bool SpellVisualData::init(const pack::PackReader& db) {
    *this = SpellVisualData{};
    db::DbcView names, kits, visuals, spells, emotes, textEmotes;
    // Effect models by id: their actors.vdpk hash.
    std::unordered_map<u32, u64> models;
    if (open(db, "SpellVisualEffectName", kEffectFields, names)) {
        models.reserve(names.recordCount());
        for (u32 r = 0; r < names.recordCount(); ++r) {
            const std::string_view path = names.getString(r, kEffectPath);
            const u64 hash = path.empty() ? 0 : fx::effectModelHash(path);
            if (hash != 0) {
                models[names.getU32(r, 0)] = hash;
                m_modelPaths.emplace(hash, path);
            }
        }
    }
    const auto model = [&](i32 id) -> u64 {
        if (id <= 0) {
            return 0;
        }
        const auto it = models.find(static_cast<u32>(id));
        return it == models.end() ? 0 : it->second;
    };
    if (open(db, "SpellVisualKit", kKitFields, kits)) {
        m_kits.reserve(kits.recordCount());
        for (u32 r = 0; r < kits.recordCount(); ++r) {
            VisualKit kit;
            kit.id = kits.getU32(r, 0);
            kit.type = kits.getU32(r, kKitType);
            const i32 animation = kits.getI32(r, kKitAnimation);
            kit.animation =
                animation >= 0 && animation < 0xFFFF ? static_cast<u16>(animation) : kNoKitAnimation;
            for (u32 slot = 0; slot < kKitSlots; ++slot) {
                kit.models[slot] = model(kits.getI32(r, kKitFirstSlot + slot));
                kit.modelCount += kit.models[slot] != 0 ? 1 : 0;
            }
            addKit(kit);
        }
    }
    if (open(db, "SpellVisual", kVisualFields, visuals)) {
        m_visuals.reserve(visuals.recordCount());
        for (u32 r = 0; r < visuals.recordCount(); ++r) {
            SpellVisualInfo v;
            v.id = visuals.getU32(r, 0);
            v.precastKit = visuals.getU32(r, kVisualPrecast);
            v.castKit = visuals.getU32(r, kVisualCast);
            v.impactKit = visuals.getU32(r, kVisualImpact);
            v.stateKit = visuals.getU32(r, kVisualState);
            v.channelKit = visuals.getU32(r, kVisualChannel);
            if (visuals.getU32(r, kVisualHasMissile) != 0) {
                v.missileModel = model(visuals.getI32(r, kVisualMissileModel));
            }
            v.missilePath = visuals.getU32(r, kVisualMissilePath);
            v.missileAttachment = visuals.getU32(r, kVisualMissileAttachment);
            if (visuals.getU32(r, kVisualHasArea) != 0) {
                v.areaModel = model(visuals.getI32(r, kVisualAreaModel));
                v.areaKit = visuals.getU32(r, kVisualAreaKit);
            }
            addVisual(v);
        }
    }
    if (open(db, "Spell", kSpellFields, spells)) {
        m_spells.reserve(spells.recordCount());
        for (u32 r = 0; r < spells.recordCount(); ++r) {
            const u32 visual = spells.getU32(r, kSpellVisual);
            const f32 speed = spells.getF32(r, kSpellSpeed);
            const u32 ranged = spells.getI32(r, kSpellItemClass) == static_cast<i32>(kItemClassWeapon)
                                   ? spells.getU32(r, kSpellItemSubclasses) & kRangedWeapons
                                   : 0;
            if (visual != 0 || speed > 0.0f || ranged != 0) {
                m_spells.push_back({spells.getU32(r, 0), visual, speed, ranged});
            }
        }
        std::sort(m_spells.begin(), m_spells.end(),
                  [](const SpellRow& a, const SpellRow& b) { return a.id < b.id; });
    }
    // Enchantments' glows: only effects ItemVisualEffects knows (some ItemVisuals rows hold garbage).
    db::DbcView itemEffects, itemVisuals, enchants;
    std::unordered_map<u32, u64> effectModels;
    if (open(db, "ItemVisualEffects", kItemEffectFields, itemEffects)) {
        for (u32 r = 0; r < itemEffects.recordCount(); ++r) {
            const std::string_view path = itemEffects.getString(r, kItemEffectPath);
            if (path.size() > 4 && path.find('.') != std::string_view::npos) {
                effectModels[itemEffects.getU32(r, 0)] = fx::effectModelHash(path);
                m_modelPaths.emplace(fx::effectModelHash(path), path);
            }
        }
    }
    std::unordered_map<u32, std::array<u64, kItemVisualSlots>> visualModels;
    if (open(db, "ItemVisuals", kItemVisualFields, itemVisuals)) {
        for (u32 r = 0; r < itemVisuals.recordCount(); ++r) {
            std::array<u64, kItemVisualSlots> slots{};
            bool any = false;
            for (u32 s = 0; s < kItemVisualSlots; ++s) {
                const auto it = effectModels.find(itemVisuals.getU32(r, kItemVisualFirst + s));
                slots[s] = it == effectModels.end() ? 0 : it->second;
                any = any || slots[s] != 0;
            }
            if (any) {
                visualModels[itemVisuals.getU32(r, 0)] = slots;
            }
        }
    }
    if (open(db, "SpellItemEnchantment", kEnchantFields, enchants)) {
        for (u32 r = 0; r < enchants.recordCount(); ++r) {
            const auto it = visualModels.find(enchants.getU32(r, kEnchantItemVisual));
            if (it != visualModels.end()) {
                m_enchantModels[enchants.getU32(r, 0)] = it->second;
            }
        }
    }
    if (open(db, "Emotes", kEmoteFields, emotes)) {
        for (u32 r = 0; r < emotes.recordCount(); ++r) {
            const u32 animation = emotes.getU32(r, kEmoteAnimation);
            m_emotes[emotes.getU32(r, 0)] = {
                animation != 0 && animation < 0xFFFF ? static_cast<u16>(animation) : kNoKitAnimation,
                emotes.getU32(r, kEmoteType) == 0};
        }
    }
    if (open(db, "EmotesText", kTextEmoteFields, textEmotes)) {
        for (u32 r = 0; r < textEmotes.recordCount(); ++r) {
            m_textEmotes[textEmotes.getU32(r, 0)] = textEmotes.getU32(r, kTextEmoteEmote);
        }
    }
    VD_LOG_INFO("Spell visuals: %zu visuals, %zu kits, %zu effect models, %zu spells with a visual",
                m_visuals.size(), m_kits.size(), models.size(), m_spells.size());
    return ready();
}

void SpellVisualData::addKit(const VisualKit& kit) {
    m_kitIndex[kit.id] = static_cast<u32>(m_kits.size());
    m_kits.push_back(kit);
}

void SpellVisualData::addVisual(const SpellVisualInfo& visual) {
    m_visualIndex[visual.id] = static_cast<u32>(m_visuals.size());
    m_visuals.push_back(visual);
}

void SpellVisualData::addSpell(u32 spellId, u32 visualId, f32 missileSpeed, u32 rangedMask) {
    const auto at = std::lower_bound(m_spells.begin(), m_spells.end(), spellId,
                                     [](const SpellRow& row, u32 id) { return row.id < id; });
    if (at != m_spells.end() && at->id == spellId) {
        *at = {spellId, visualId, missileSpeed, rangedMask};
    } else {
        m_spells.insert(at, {spellId, visualId, missileSpeed, rangedMask});
    }
}

const SpellVisualData::SpellRow* SpellVisualData::spell(u32 spellId) const {
    const auto at = std::lower_bound(m_spells.begin(), m_spells.end(), spellId,
                                     [](const SpellRow& row, u32 id) { return row.id < id; });
    return at != m_spells.end() && at->id == spellId ? &*at : nullptr;
}

const VisualKit* SpellVisualData::kit(u32 id) const {
    const auto it = id == 0 ? m_kitIndex.end() : m_kitIndex.find(id);
    return it == m_kitIndex.end() ? nullptr : &m_kits[it->second];
}

const SpellVisualInfo* SpellVisualData::visual(u32 id) const {
    const auto it = id == 0 ? m_visualIndex.end() : m_visualIndex.find(id);
    return it == m_visualIndex.end() ? nullptr : &m_visuals[it->second];
}

const SpellVisualInfo* SpellVisualData::spellVisual(u32 spellId) const {
    const SpellRow* row = spell(spellId);
    return row ? visual(row->visual) : nullptr;
}

f32 SpellVisualData::missileSpeed(u32 spellId) const {
    const SpellRow* row = spell(spellId);
    return row ? row->speed : 0.0f;
}

u32 SpellVisualData::rangedWeaponMask(u32 spellId) const {
    const SpellRow* row = spell(spellId);
    return row ? row->rangedMask : 0;
}

std::string_view SpellVisualData::modelPath(u64 model) const {
    const auto it = m_modelPaths.find(model);
    return it == m_modelPaths.end() ? std::string_view() : it->second;
}

bool SpellVisualData::enchantModels(u32 enchantmentId, std::array<u64, kItemVisualSlots>& out) const {
    const auto it = enchantmentId == 0 ? m_enchantModels.end() : m_enchantModels.find(enchantmentId);
    if (it == m_enchantModels.end()) {
        return false;
    }
    out = it->second;
    return true;
}

u16 SpellVisualData::emoteAnimation(u32 emoteId) const {
    const auto it = m_emotes.find(emoteId);
    return it == m_emotes.end() ? kNoKitAnimation : it->second.animation;
}

bool SpellVisualData::emoteOneShot(u32 emoteId) const {
    const auto it = m_emotes.find(emoteId);
    return it != m_emotes.end() && it->second.oneShot;
}

u32 SpellVisualData::textEmote(u32 emotesTextId) const {
    const auto it = m_textEmotes.find(emotesTextId);
    return it == m_textEmotes.end() ? 0 : it->second;
}

} // namespace vd::game
