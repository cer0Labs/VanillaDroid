#include "game/TradeSkills.h"

#include "core/Hash.h"
#include "core/Profile.h"
#include "game/Abilities.h"
#include "game/Entities.h"
#include "game/Inventory.h"
#include "game/Queries.h"
#include "game/Spells.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cctype>

namespace vd::game {
namespace {

namespace uf = net::uf;

constexpr u32 kEffectLearnSpell = 36;    // SPELL_EFFECT_LEARN_SPELL (Beast Training: to the pet, target 5)
constexpr u32 kEffectLearnPetSpell = 57; // SPELL_EFFECT_LEARN_PET_SPELL
constexpr u32 kTargetPet = 5;            // TARGET_UNIT_CASTER_PET
constexpr u32 kSpellAttrPassive = 0x40;  // SPELL_ATTR_PASSIVE: never cast (Spell_C_CastSpell returns first)
constexpr u16 kCastFlagsNoRecast = 0x41; // SMSG_SPELL_GO cast flags that keep DoRecast from running
constexpr u32 kNonEquipSlot = 23;        // NONEQUIPSLOT

// WoW.exe's inventory type -> slot bits table (0x809200), with one ring, one trinket and the first bag slot
// (CGTradeSkillInfo overrides those three).
constexpr std::array<u32, 29> kSlotMasks = {
    0x0,   0x1,    0x2,     0x4,     0x8,     0x10,    0x20,    0x40,   0x80,    0x100,
    0x200, 0x400,  0x1000,  0x18000, 0x10000, 0x20000, 0x4000,  0x8000, 0x80000, 0x40000,
    0x10,  0x8000, 0x10000, 0x10000, 0x0,     0x20000, 0x20000, 0x0,    0x20000};

// Storm's case-insensitive compare, as the client sorts names.
bool lessNoCase(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

void raceClass(const Entity* e, u32& race, u32& cls) {
    race = e ? e->byteValue(uf::UNIT_FIELD_BYTES_0, 0) : 0;
    cls = e ? e->byteValue(uf::UNIT_FIELD_BYTES_0, 1) : 0;
}

} // namespace

const std::array<const char*, kTradeSlots> kTradeSlotKeys = {
    "HEADSLOT",     "NECKSLOT",     "SHOULDERSLOT", "SHIRTSLOT",    "CHESTSLOT",         "WAISTSLOT",
    "LEGSSLOT",     "FEETSLOT",     "WRISTSLOT",    "HANDSSLOT",    "FINGER0SLOT",       "FINGER1SLOT",
    "TRINKET0SLOT", "TRINKET1SLOT", "BACKSLOT",     "MAINHANDSLOT", "SECONDARYHANDSLOT", "RANGEDSLOT",
    "TABARDSLOT",   "BAGSLOT",      "BAGSLOT",      "BAGSLOT",      "BAGSLOT",           "NONEQUIPSLOT"};

u32 tradeSlotMask(u32 inventoryType) {
    const u32 mask = inventoryType < kSlotMasks.size() ? kSlotMasks[inventoryType] : 0;
    return mask != 0 ? mask : 1u << kNonEquipSlot;
}

RecipeColor recipeColor(u32 rank, u32 trivialLow, u32 trivialHigh) {
    // CGTradeSkillInfo (WoW.exe): signed arithmetic, the yellow start defaulting to 25 below grey.
    const i32 high = static_cast<i32>(trivialHigh);
    const i32 low = trivialLow != 0 ? static_cast<i32>(trivialLow) : high - 25;
    const i32 value = static_cast<i32>(rank);
    if (value < low) {
        return RecipeColor::Optimal;
    }
    if (value < (low + high) / 2) {
        return RecipeColor::Medium;
    }
    return value < high ? RecipeColor::Easy : RecipeColor::Trivial;
}

TradeSkills::TradeSkills(const EntityStore& entities, QueryCache& queries, const Inventory& inventory,
                         const SpellBook& spells, const Abilities& abilities)
    : m_entities(entities), m_queries(queries), m_inventory(inventory), m_spells(spells),
      m_abilities(abilities) {
    m_recipes.reserve(256);
    m_headers.reserve(32);
    m_saved.reserve(32);
    m_rows.reserve(300);
    m_known.reserve(256);
    m_crafts.reserve(256);
    m_craftRows.reserve(256);
    m_collapsedLines.reserve(8);
}

void TradeSkills::enterWorld(u64 player, bool sameCharacter) {
    m_player = player;
    closeTradeSkill(); // TradeSkillFrame: PLAYER_ENTERING_WORLD hides it
    if (!sameCharacter) {
        closeCraft();
        m_lastSkillLine = 0;
        m_headers.clear();
        m_selected = 0;
    }
}

void TradeSkills::leaveWorld() {
    closeTradeSkill();
    closeCraft();
}

const Entity* TradeSkills::player() const {
    return m_player != 0 ? m_entities.find(m_player) : nullptr;
}

TradeSkills::PlayerSkill TradeSkills::skill(u32 skillLine) const {
    PlayerSkill s;
    const Entity* me = player();
    if (!me || skillLine == 0) {
        return s;
    }
    for (u32 i = 0; i < 128; ++i) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + i * 3);
        if ((me->value(base) & 0xFFFFu) != skillLine) {
            continue;
        }
        const u32 values = me->value(static_cast<u16>(base + 1));
        s.value = values & 0xFFFFu;
        s.max = values >> 16;
        s.permBonus = me->value(static_cast<u16>(base + 2)) >> 16;
        s.known = true;
        break;
    }
    return s;
}

bool TradeSkills::cast(u32 spellId) {
    return m_caster.cast && m_caster.cast(m_caster.context, spellId);
}

void TradeSkills::stopRecast() {
    if (m_recastSpell != 0) {
        m_recastSpell = 0; // UPDATE_TRADESKILL_RECAST
        m_recastCount = 0;
        touch();
    }
}

// --- Opening
// -------------------------------------------------------------------------------------------------

bool TradeSkills::openBySpell(u32 spellId) {
    TradeSpellInfo s;
    if (!m_data || !m_data->spell(spellId, s) || !s.opensWindow() || (s.attributes & kSpellAttrPassive)) {
        return false;
    }
    if (s.misc[0] != 0) {
        openCraft(static_cast<u32>(s.misc[0]), spellId);
        return true;
    }
    u32 race = 0, cls = 0;
    raceClass(player(), race, cls);
    SkillAbility a;
    if (m_data->ability(spellId, race, cls, a) && a.skillLine != 0) {
        openTradeSkill(a.skillLine);
    }
    return true;
}

void TradeSkills::update(u32 nowMs) {
    (void)nowMs;
    if (!m_data || (m_skillLine == 0 && m_craftList == 0)) {
        return;
    }
    VD_PROFILE_ZONE("TradeSkills::update");
    const bool known = m_spells.knownVersion() != m_knownVersion;
    const bool bags = m_inventory.version() != m_inventoryVersion;
    const bool items = m_queries.version() != m_queryVersion;
    if (m_skillLine != 0) {
        const PlayerSkill s = skill(m_skillLine);
        const u32 rank = s.value > 0 ? s.value + s.permBonus : s.value;
        const u32 max = s.max > 0 ? s.max + s.permBonus : s.max;
        if (rank != m_skillRank || max != m_skillMax) {
            m_skillRank = rank;
            m_skillMax = max;
            touch();
        }
        if (known || bags || (items && m_pendingItems > 0) || s.value != m_rankSeen) {
            rebuildTrade();
        }
    }
    if (m_craftList != 0) {
        const u64 pet = petSignature();
        if (known || bags || pet != m_petSeen || skill(m_craftSkillLine).value != m_craftRankSeen) {
            rebuildCraft();
        }
    }
}

// --- TradeSkillFrame
// -----------------------------------------------------------------------------------------

void TradeSkills::openTradeSkill(u32 line) {
    if (line == m_skillLine) {
        closeTradeSkill(); // the profession's spell again: TRADE_SKILL_CLOSE
        return;
    }
    if (line != m_lastSkillLine) {
        // Another profession than the last one shown: its filters and selection start over.
        m_headers.clear();
        m_subclassFilter = -1;
        m_slotFilter = -1;
        m_selected = 0;
    }
    m_skillLine = line;
    m_lastSkillLine = line;
    rebuildTrade();
    if (m_known.empty()) {
        m_skillLine = 0; // no recipe known: TRADE_SKILL_SHOW never comes
        return;
    }
    touch();
}

void TradeSkills::closeTradeSkill() {
    if (m_skillLine == 0) {
        return;
    }
    m_skillLine = 0; // CloseTradeSkill: the repeat stops, TRADE_SKILL_CLOSE
    stopRecast();
    touch();
}

std::string_view TradeSkills::skillLineName() const {
    return m_data ? m_data->skillLineName(m_skillLine) : std::string_view{};
}

u32 TradeSkills::available(const TradeSpellInfo& spell) const {
    // numAvailable: the fewest batches any reagent allows (0 without reagents).
    i32 fewest = -1;
    for (u32 i = 0; i < spell.reagents.size(); ++i) {
        if (spell.reagents[i] == 0 || spell.reagentCounts[i] == 0) {
            continue;
        }
        const i32 batches = static_cast<i32>(m_inventory.count(spell.reagents[i]) / spell.reagentCounts[i]);
        fewest = fewest < 0 ? batches : std::min(fewest, batches);
    }
    return fewest > 0 ? static_cast<u32>(fewest) : 0;
}

void TradeSkills::rebuildTrade() {
    VD_PROFILE_ZONE("TradeSkills::rebuildTrade");
    m_knownVersion = m_spells.knownVersion();
    m_inventoryVersion = m_inventory.version();
    m_queryVersion = m_queries.version();
    const PlayerSkill playerSkill = skill(m_skillLine);
    m_rankSeen = playerSkill.value;
    u32 race = 0, cls = 0;
    raceClass(player(), race, cls);

    // The known recipes of the line (the player's per skill line list).
    m_known.clear();
    TradeSpellInfo spell;
    SkillAbility ability;
    for (const u32 id : m_spells.known()) {
        if (m_data->spell(id, spell) && spell.tradeSpell() && m_data->ability(id, race, cls, ability) &&
            ability.skillLine == m_skillLine) {
            m_known.push_back(id);
        }
    }

    // Headers keep their states across rebuilds (by class and subclass), the subclass filter too.
    m_saved.assign(m_headers.begin(), m_headers.end());
    const bool filtered = m_subclassFilter >= 0 && static_cast<usize>(m_subclassFilter) < m_saved.size();
    const u32 filterClass = filtered ? m_saved[static_cast<usize>(m_subclassFilter)].itemClass : 0;
    const u32 filterSubclass = filtered ? m_saved[static_cast<usize>(m_subclassFilter)].subclass : 0;
    m_headers.clear();
    m_recipes.clear();
    m_slotsPresent = 0;
    m_pendingItems = 0;
    for (const u32 id : m_known) {
        if (!m_data->spell(id, spell)) {
            continue;
        }
        const net::ItemQueryInfo* item = spell.createdItem != 0 ? m_queries.item(spell.createdItem) : nullptr;
        if (spell.createdItem != 0 && !item) {
            ++m_pendingItems; // asked for: the list is built when every template is here
            continue;
        }
        TradeRecipe r;
        r.spellId = id;
        r.name = spell.name;
        r.color = m_data->ability(id, race, cls, ability)
                      ? recipeColor(playerSkill.value, ability.trivialLow, ability.trivialHigh)
                      : RecipeColor::Optimal;
        const u32 itemClass = item ? item->itemClass : 0;
        const u32 subclass = item ? item->subclass : 0;
        r.slotMask = tradeSlotMask(item ? item->inventoryType : 0);
        r.itemLevel = item ? item->itemLevel : 0;
        r.available = available(spell);
        m_slotsPresent |= r.slotMask;
        u32 h = 0;
        while (h < m_headers.size() &&
               (m_headers[h].itemClass != itemClass || m_headers[h].subclass != subclass)) {
            ++h;
        }
        if (h == m_headers.size()) {
            TradeHeader header;
            header.itemClass = itemClass;
            header.subclass = subclass;
            header.name = m_data->subclassName(itemClass, subclass);
            for (const TradeHeader& old : m_saved) {
                if (old.itemClass == itemClass && old.subclass == subclass) {
                    header.collapsed = old.collapsed;
                }
            }
            m_headers.push_back(header);
        }
        r.header = h;
        m_recipes.push_back(r);
    }
    if (m_pendingItems > 0) {
        m_recipes.clear();
        m_headers.assign(m_saved.begin(), m_saved.end()); // their states survive the wait
        m_rows.clear();
        touch();
        return;
    }

    // Headers by item class, then by name; recipes by header, colour, item level (highest first), name.
    m_saved.assign(m_headers.begin(), m_headers.end());
    std::sort(m_headers.begin(), m_headers.end(), [](const TradeHeader& a, const TradeHeader& b) {
        return a.itemClass != b.itemClass ? a.itemClass < b.itemClass : lessNoCase(a.name, b.name);
    });
    m_subclassFilter = -1;
    for (TradeRecipe& r : m_recipes) {
        const TradeHeader& old = m_saved[r.header];
        for (u32 h = 0; h < m_headers.size(); ++h) {
            if (m_headers[h].itemClass == old.itemClass && m_headers[h].subclass == old.subclass) {
                r.header = h;
            }
        }
    }
    for (u32 h = 0; filtered && h < m_headers.size(); ++h) {
        if (m_headers[h].itemClass == filterClass && m_headers[h].subclass == filterSubclass) {
            m_subclassFilter = static_cast<i32>(h);
        }
    }
    std::sort(m_recipes.begin(), m_recipes.end(), [](const TradeRecipe& a, const TradeRecipe& b) {
        if (a.header != b.header) {
            return a.header < b.header;
        }
        if (a.color != b.color) {
            return a.color < b.color;
        }
        if (a.itemLevel != b.itemLevel) {
            return a.itemLevel > b.itemLevel;
        }
        return lessNoCase(a.name, b.name);
    });
    if (m_slotFilter >= 0 && (m_slotsPresent & (1u << m_slotFilter)) == 0) {
        m_slotFilter = -1;
    }
    flattenTrade();
}

void TradeSkills::flattenTrade() {
    for (TradeHeader& h : m_headers) {
        h.count = 0;
    }
    const u32 slotBit = m_slotFilter >= 0 ? 1u << m_slotFilter : 0xFFFFFFFFu;
    for (const TradeRecipe& r : m_recipes) {
        if (r.slotMask & slotBit) {
            ++m_headers[r.header].count;
        }
    }
    m_rows.clear();
    usize next = 0;
    for (u32 h = 0; h < m_headers.size(); ++h) {
        const usize first = next;
        while (next < m_recipes.size() && m_recipes[next].header == h) {
            ++next;
        }
        const TradeHeader& header = m_headers[h];
        if ((m_subclassFilter >= 0 && static_cast<u32>(m_subclassFilter) != h) || header.count == 0) {
            continue;
        }
        m_rows.push_back({true, h});
        if (header.collapsed) {
            continue;
        }
        for (usize i = first; i < next; ++i) {
            if (m_recipes[i].slotMask & slotBit) {
                m_rows.push_back({false, static_cast<u32>(i)});
            }
        }
    }
    // TRADE_SKILL_UPDATE: the selection stays while shown, else the first recipe (GetFirstTradeSkill).
    if (selectedRow() < 0) {
        m_selected = 0;
        for (const TradeSkillRow& row : m_rows) {
            if (!row.header) {
                m_selected = m_recipes[row.index].spellId;
                break;
            }
        }
    }
    touch();
}

i32 TradeSkills::selectedRow() const {
    for (u32 i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].header && m_recipes[m_rows[i].index].spellId == m_selected && m_selected != 0) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

const TradeRecipe* TradeSkills::selectedRecipe() const {
    const i32 row = selectedRow();
    return row >= 0 ? &m_recipes[m_rows[static_cast<u32>(row)].index] : nullptr;
}

void TradeSkills::selectRow(u32 row) {
    if (row >= m_rows.size()) {
        return;
    }
    if (m_rows[row].header) {
        TradeHeader& h = m_headers[m_rows[row].index];
        h.collapsed = !h.collapsed; // CollapseTradeSkillSubClass / ExpandTradeSkillSubClass
        flattenTrade();
        return;
    }
    m_selected = m_recipes[m_rows[row].index].spellId; // SelectTradeSkill
    touch();
}

bool TradeSkills::allCollapsed() const {
    return std::all_of(m_headers.begin(), m_headers.end(), [](const TradeHeader& h) { return h.collapsed; });
}

void TradeSkills::setAllCollapsed(bool collapsed) {
    for (TradeHeader& h : m_headers) {
        h.collapsed = collapsed;
    }
    flattenTrade();
}

void TradeSkills::setSubclassFilter(i32 header) {
    m_subclassFilter = header >= 0 && static_cast<usize>(header) < m_headers.size() ? header : -1;
    flattenTrade();
}

void TradeSkills::setSlotFilter(i32 slot) {
    m_slotFilter =
        slot >= 0 && slot < static_cast<i32>(kTradeSlots) && (m_slotsPresent & (1u << slot)) ? slot : -1;
    flattenTrade();
}

u32 TradeSkills::repeatCount() const {
    return m_recastSpell != 0 && m_recastSpell == m_selected && m_recastCount > 1 ? m_recastCount : 1;
}

bool TradeSkills::create(u32 count) {
    const TradeRecipe* r = selectedRecipe();
    if (!r) {
        return false;
    }
    const u32 times = std::min(count, r->available); // DoTradeSkill: no more than the reagents allow
    if (times > 1) {
        m_recastSpell = r->spellId;
        m_recastCount = times;
        touch(); // UPDATE_TRADESKILL_RECAST
    }
    return cast(r->spellId);
}

void TradeSkills::onSpellGo(const net::SpellGo& m, u64 playerGuid) {
    if (m.caster != playerGuid || m.unitCaster != playerGuid || (m.castFlags & kCastFlagsNoRecast) != 0) {
        return;
    }
    // CGTradeSkillInfo::DoRecast: the repeated recipe goes again until the count runs out; any other spell of
    // the player's ends the repeat.
    if (m_recastSpell != 0 && m_recastCount != 0 && m_recastSpell == m.spellId) {
        if (--m_recastCount == 0) {
            touch();
            return;
        }
        cast(m.spellId);
        touch();
        return;
    }
    stopRecast();
}

void TradeSkills::onCastFailed(u32 spellId) {
    if (spellId == m_recastSpell) {
        stopRecast();
    }
}

// --- CraftFrame
// ----------------------------------------------------------------------------------------------

void TradeSkills::openCraft(u32 list, u32 opener) {
    if (list == m_craftList) {
        closeCraft(); // CRAFT_CLOSE
        return;
    }
    m_craftList = list;
    m_craftOpener = opener;
    m_craftSelected = 0;
    m_collapsedLines.clear();
    rebuildCraft();
}

void TradeSkills::closeCraft() {
    if (m_craftList == 0) {
        return;
    }
    m_craftList = 0;
    touch();
}

std::array<u32, 2> TradeSkills::petSkillLines() const {
    const PetState& pet = m_abilities.pet();
    const Entity* e = pet.active() ? m_entities.find(pet.guid) : nullptr;
    if (!e || !m_data) {
        return {};
    }
    const net::CreatureQueryInfo* info = m_queries.creature(e->value(uf::OBJECT_FIELD_ENTRY));
    return info ? m_data->familySkillLines(info->family) : std::array<u32, 2>{};
}

bool TradeSkills::petKnows(u32 spellId) const {
    for (const u32 packed : m_abilities.pet().spells) {
        if ((packed & 0x00FFFFFFu) == spellId) {
            return true;
        }
    }
    return false;
}

bool TradeSkills::petKnowsHigherRank(u32 spellId) const {
    // The ranks after it (SkillLineAbility's superceded-by chain in the pet's lines).
    const std::array<u32, 2> lines = petSkillLines();
    SkillAbility a;
    u32 next = spellId;
    for (u32 guard = 0; guard < 16; ++guard) {
        bool found = false;
        for (const u32 line : lines) {
            if (line != 0 && m_data->abilityInLine(line, next, a)) {
                found = true;
                break;
            }
        }
        if (!found || a.supercededBy == 0) {
            return false;
        }
        next = a.supercededBy;
        if (petKnows(next)) {
            return true;
        }
    }
    return false;
}

u64 TradeSkills::petSignature() const {
    const PetState& pet = m_abilities.pet();
    u64 hash = kFnvOffset;
    const auto mix = [&](u64 value) {
        hash ^= value;
        hash *= kFnvPrime;
    };
    mix(pet.guid);
    for (const u32 packed : pet.spells) {
        mix(packed & 0x00FFFFFFu);
    }
    if (const Entity* e = pet.active() ? m_entities.find(pet.guid) : nullptr) {
        mix(e->value(uf::UNIT_FIELD_LEVEL));
        mix(e->value(uf::UNIT_TRAINING_POINTS));
        mix(e->value(uf::OBJECT_FIELD_ENTRY));
        mix(petSkillLines()[0]); // its template's family, once the server told it
    }
    return hash;
}

RecipeColor TradeSkills::craftColor(const TradeSpellInfo& spell, const SkillAbility* ability,
                                    u32 rank) const {
    if (ability && ability->trivialHigh != 0) {
        return recipeColor(rank, ability->trivialLow, ability->trivialHigh);
    }
    // Without ranks: "used" once whoever it teaches knows the ability (or a higher rank, for the pet).
    for (u32 i = 0; i < 3; ++i) {
        if (spell.effects[i] != kEffectLearnSpell) {
            continue;
        }
        const u32 taught = spell.triggerSpells[i];
        if (spell.implicitTargetA[i] != kTargetPet && spell.implicitTargetB[i] != kTargetPet) {
            if (m_spells.knows(taught)) {
                return RecipeColor::Used;
            }
        } else if (m_abilities.pet().active() && (petKnows(taught) || petKnowsHigherRank(taught))) {
            return RecipeColor::Used;
        }
        break;
    }
    return RecipeColor::None;
}

void TradeSkills::rebuildCraft() {
    VD_PROFILE_ZONE("TradeSkills::rebuildCraft");
    m_knownVersion = m_spells.knownVersion();
    m_inventoryVersion = m_inventory.version();
    m_queryVersion = m_queries.version();
    m_petSeen = petSignature();
    u32 race = 0, cls = 0;
    raceClass(player(), race, cls);
    m_crafts.clear();
    m_craftSkillLine = 0;
    TradeSpellInfo spell;
    SkillAbility ability;
    u32 lines[8] = {};
    u32 lineCount = 0;
    u32 rankLine = 0, rank = 0; // the player's rank in the last line looked up
    for (const u32 id : m_spells.known()) {
        if (!m_data->spell(id, spell) || spell.castUi != m_craftList) {
            continue;
        }
        CraftEntry e;
        e.spellId = id;
        const bool inLine = m_data->ability(id, race, cls, ability);
        e.skillLine = inLine ? ability.skillLine : 0;
        if (e.skillLine != rankLine) {
            rankLine = e.skillLine;
            rank = skill(rankLine).value;
        }
        e.name = spell.name;
        e.rank = spell.rank;
        e.color = craftColor(spell, inLine ? &ability : nullptr, rank);
        e.available = available(spell);
        e.spellLevel = spell.spellLevel;
        if (m_crafts.empty() && m_craftList == kCraftEnchanting) {
            m_craftSkillLine = e.skillLine; // GetCraftDisplaySkillLine: the first craft's line
        }
        bool seen = false;
        for (u32 i = 0; i < lineCount; ++i) {
            seen = seen || lines[i] == e.skillLine;
        }
        if (!seen && lineCount < 8) {
            lines[lineCount++] = e.skillLine;
        }
        m_crafts.push_back(e);
    }
    // Headers only when the crafts span several skill lines.
    m_craftHeaders = lineCount > 1 ? lineCount : 0;
    for (u32 i = 0; i < m_craftHeaders; ++i) {
        CraftEntry h;
        h.skillLine = lines[i];
        h.name = m_data->skillLineName(lines[i]);
        h.collapsed =
            std::find(m_collapsedLines.begin(), m_collapsedLines.end(), lines[i]) != m_collapsedLines.end();
        m_crafts.push_back(h);
    }
    // By header (skill line name), the header first; then colour and name (Beast Training: then the ability's
    // level, ranks in order).
    const bool beasts = m_craftList == kCraftBeastTraining;
    const TradeSkillData* data = m_data;
    std::sort(m_crafts.begin(), m_crafts.end(), [&](const CraftEntry& a, const CraftEntry& b) {
        if (a.skillLine != b.skillLine) {
            return lessNoCase(data->skillLineName(a.skillLine), data->skillLineName(b.skillLine));
        }
        if ((a.spellId == 0) != (b.spellId == 0)) {
            return a.spellId == 0;
        }
        if (a.color != b.color) {
            return a.color < b.color;
        }
        if (a.name != b.name) {
            return lessNoCase(a.name, b.name);
        }
        return beasts && a.spellLevel < b.spellLevel;
    });
    m_craftRankSeen = skill(m_craftSkillLine).value;
    if (m_craftSkillLine != 0) {
        const PlayerSkill s = skill(m_craftSkillLine);
        m_craftSkillRank = s.value > 0 ? s.value + s.permBonus : s.value;
        m_craftSkillMax = s.max > 0 ? s.max + s.permBonus : s.max;
    }
    flattenCraft();
}

void TradeSkills::flattenCraft() {
    m_craftRows.clear();
    bool collapsed = false;
    for (const CraftEntry& e : m_crafts) {
        if (e.spellId == 0) {
            collapsed = e.collapsed;
            m_craftRows.push_back(e);
        } else if (!collapsed) {
            m_craftRows.push_back(e);
        }
    }
    // CRAFT_UPDATE: the selection stays while shown, else the first craft (the second line under a header).
    if (selectedCraftRow() < 0) {
        m_craftSelected = 0;
        for (const CraftEntry& e : m_craftRows) {
            if (e.spellId != 0) {
                m_craftSelected = e.spellId;
                break;
            }
        }
    }
    touch();
}

i32 TradeSkills::selectedCraftRow() const {
    for (u32 i = 0; i < m_craftRows.size(); ++i) {
        if (m_craftRows[i].spellId != 0 && m_craftRows[i].spellId == m_craftSelected) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void TradeSkills::selectCraftRow(u32 row) {
    if (row >= m_craftRows.size()) {
        return;
    }
    const CraftEntry& e = m_craftRows[row];
    if (e.spellId != 0) {
        m_craftSelected = e.spellId; // SelectCraft
        touch();
        return;
    }
    const auto it = std::find(m_collapsedLines.begin(), m_collapsedLines.end(), e.skillLine);
    if (it != m_collapsedLines.end()) {
        m_collapsedLines.erase(it); // ExpandCraftSkillLine
    } else {
        m_collapsedLines.push_back(e.skillLine); // CollapseCraftSkillLine
    }
    for (CraftEntry& c : m_crafts) {
        if (c.spellId == 0 && c.skillLine == e.skillLine) {
            c.collapsed = !c.collapsed;
        }
    }
    flattenCraft();
}

bool TradeSkills::craftsAllCollapsed() const {
    for (const CraftEntry& e : m_crafts) {
        if (e.spellId == 0 && !e.collapsed) {
            return false;
        }
    }
    return true;
}

void TradeSkills::setCraftsAllCollapsed(bool collapsed) {
    m_collapsedLines.clear();
    for (CraftEntry& e : m_crafts) {
        if (e.spellId == 0) {
            e.collapsed = collapsed;
            if (collapsed) {
                m_collapsedLines.push_back(e.skillLine);
            }
        }
    }
    flattenCraft();
}

bool TradeSkills::craftPetCost(u32 spellId, u32& trainingPoints, u32& requiredLevel) const {
    trainingPoints = 0;
    requiredLevel = 0;
    TradeSpellInfo spell;
    if (!m_data || !m_abilities.pet().active() || !m_data->spell(spellId, spell)) {
        return false;
    }
    const std::array<u32, 2> lines = petSkillLines();
    bool found = false;
    for (u32 i = 0; i < 3; ++i) {
        const bool teachesPet =
            (spell.effects[i] == kEffectLearnSpell && spell.implicitTargetA[i] == kTargetPet) ||
            spell.effects[i] == kEffectLearnPetSpell;
        if (!teachesPet) {
            continue;
        }
        const u32 taught = spell.triggerSpells[i];
        SkillAbility a;
        for (const u32 line : lines) {
            if (line != 0 && m_data->abilityInLine(line, taught, a)) {
                TradeSpellInfo learned;
                trainingPoints = a.trainingPoints; // the row's cost as the client shows it
                requiredLevel = m_data->spell(taught, learned) ? learned.spellLevel : 0;
                found = true;
                break;
            }
        }
    }
    return found;
}

PetTrainingPoints TradeSkills::petTrainingPoints() const {
    const PetState& pet = m_abilities.pet();
    const Entity* e = pet.active() ? m_entities.find(pet.guid) : nullptr;
    if (!e) {
        return {};
    }
    const u32 field = e->value(uf::UNIT_TRAINING_POINTS);
    return {field >> 16, field & 0xFFFFu};
}

} // namespace vd::game
