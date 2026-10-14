#include "app/PetScreens.h"

#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/ModelViews.h"
#include "app/Portraits.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;
using ui::rgba;

constexpr u32 kClassHunter = 3;
constexpr f32 kStableReach = 10.0f; // as the trainer's and vendor's windows (INTERACTION_DISTANCE)
constexpr f32 kPetZoom = 1.4f;      // a beast is wider than tall: the frame's camera closer
constexpr u32 kErrorColor = rgba(255, 26, 26);

f32 floatField(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

i32 intField(const Entity& e, u16 index) {
    return static_cast<i32>(e.value(index));
}

// PaperDollFrame's colors for a value: green when buffed, red when debuffed (or negative), white otherwise.
u32 valueColor(i32 positive, i32 negative) {
    return negative < 0 ? ui::kRed : positive > 0 ? ui::kGreen : ui::kWhite;
}

const Entity* petEntity(GameSession& session) {
    const game::PetState& pet = session.play().abilities().pet();
    return pet.active() ? session.entities().find(pet.guid) : nullptr;
}

// HasPetUI's second value (isHunterPet / canGainXP): the hunter's tamed pet.
bool hunterPet(GameSession& session, const Entity* pet) {
    const Entity* me = session.entities().find(session.playerGuid());
    return pet && me && pet->value(uf::UNIT_FIELD_PETNUMBER) != 0 &&
           me->byteValue(uf::UNIT_FIELD_BYTES_0, 1) == kClassHunter;
}

} // namespace

void PetScreens::init(const game::GameData* data, MessageSink sink, void* sinkContext) {
    m_data = data;
    m_sink = sink;
    m_sinkContext = sinkContext;
    m_stableLook.skin.reserve(8);
    m_stableLook.geosets.reserve(32);
}

void PetScreens::message(u32 color, const char* text) {
    if (m_sink) {
        m_sink(m_sinkContext, color, false, text);
    }
}

void PetScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("PetScreens::update");
    m_nowMs = nowMs;
    if (!session.inWorld()) {
        return;
    }
    game::Gameplay& play = session.play();
    if (m_player != session.playerGuid()) {
        m_player = session.playerGuid();
        m_eventCursor = play.events().end();
        m_combatCursor = play.combatLog().end();
        m_playerFeedback.clear();
        m_petFeedback.clear();
        m_stableShown = false;
    }
    readEvents(session);
    readCombat(session);
    // The stable closes when its master is gone or left behind (as the trainer's window).
    game::PetStable& stable = play.stable();
    if (stable.open()) {
        const Entity* me = session.entities().find(session.playerGuid());
        const Entity* npc = session.entities().find(stable.stableMaster());
        if (!me || !npc ||
            game::distanceAt(*me, *npc, nowMs) - floatField(*npc, uf::UNIT_FIELD_BOUNDINGRADIUS) >
                kStableReach) {
            stable.close();
        }
    }
    // PetStableFrame's OnShow / OnHide sounds.
    if (stable.open() != m_stableShown) {
        m_uiSound(stable.open() ? UiSound::CharacterOpen : UiSound::CharacterClose);
        m_stableShown = stable.open();
    }
}

void PetScreens::readEvents(GameSession& session) {
    game::Gameplay& play = session.play();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind == game::GameEventKind::StableFailed) {
            message(kErrorColor, text::get("ERR_NOT_ENOUGH_MONEY").data()); // the result handler's error 37
        }
    }
}

void PetScreens::readCombat(GameSession& session) {
    game::Gameplay& play = session.play();
    const u64 me = session.playerGuid();
    const u64 pet = play.abilities().pet().guid;
    // Spell.dbc attributes 0x100: hidden from the combat log, no UNIT_COMBAT for its damage and misses.
    game::SpellLogFilter hidden;
    hidden.context = m_data;
    hidden.hidden = [](const void* context, u32 spellId) {
        const game::GameData* data = static_cast<const game::GameData*>(context);
        const std::optional<game::SpellInfo> info = data ? data->spell(spellId) : std::nullopt;
        return info && (info->attributes & 0x100) != 0;
    };
    std::array<game::UnitCombat, 2> fired{};
    while (const game::CombatEvent* e = play.combatLog().next(m_combatCursor)) {
        const u32 n = game::unitCombats(*e, hidden, fired);
        for (u32 i = 0; i < n; ++i) {
            if (fired[i].unit == me) { // UNIT_COMBAT "player": PlayerFrame
                m_playerFeedback.onCombatEvent(fired[i], m_nowMs);
            } else if (pet != 0 && fired[i].unit == pet) { // "pet": PetFrame
                m_petFeedback.onCombatEvent(fired[i], m_nowMs);
            }
        }
    }
}

u32 PetScreens::familyOf(GameSession& session, u32 entry) const {
    const net::CreatureQueryInfo* info = entry ? session.creature(entry) : nullptr;
    return info ? info->family : 0u;
}

std::string_view PetScreens::levelText(Text& out, std::string_view name, u32 level, u32 family) const {
    // format(UNIT_LEVEL_TEMPLATE, level).." "..family, after the name and a space (PetStableLevelText).
    char levelPart[32];
    const std::string_view lvl = text::format(levelPart, "UNIT_LEVEL_TEMPLATE", level);
    const std::string_view familyName = m_pets ? m_pets->familyName(family) : std::string_view{};
    std::snprintf(out.data(), out.size(), "%.*s%s%.*s%s%.*s", static_cast<int>(name.size()), name.data(),
                  name.empty() ? "" : " ", static_cast<int>(lvl.size()), lvl.data(),
                  familyName.empty() ? "" : " ", static_cast<int>(familyName.size()), familyName.data());
    return out.data();
}

// --- The stable --------------------------------------------------------------------------------------------

bool PetScreens::stableOpen(const GameSession& session) {
    return session.play().stable().open();
}

void PetScreens::closeStable(GameSession& session) {
    session.play().stable().close();
}

const game::ActorLook* PetScreens::stableLook(GameSession& session, i32 index, u64 petGuid) {
    // SetPetStablePaperdoll: the pet out when the current slot is selected, else the stabled one's creature.
    if (index == 0 && petGuid != 0 && m_portraits) {
        if (const game::ActorLook* look = m_portraits->look(petGuid)) {
            return look;
        }
    }
    const game::StabledPet* p = index >= 0 ? session.play().stable().pet(static_cast<u32>(index)) : nullptr;
    const net::CreatureQueryInfo* info = p ? session.creature(p->entry) : nullptr;
    const game::AppearanceDb* db = m_portraits ? m_portraits->appearance() : nullptr;
    if (!info || info->displayId == 0 || !db) {
        return nullptr;
    }
    if (info->displayId != m_stableDisplay) { // built when the shown pet changes, not every frame
        m_stableLook = db->creature(info->displayId);
        m_stableDisplay = info->displayId;
    }
    return &m_stableLook;
}

void PetScreens::drawStable(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("PetScreens::stable");
    m_nowMs = nowMs;
    game::Gameplay& play = session.play();
    game::PetStable& stable = play.stable();
    if (!stable.open()) {
        return;
    }
    const Entity* pet = petEntity(session);
    const bool isHunterPet = hunterPet(session, pet);
    StableView& v = m_stableView;
    v = StableView{};
    const u32 money = play.inventory().money();
    v.money = money;
    v.nextSlotCost = m_pets ? m_pets->stableSlotPrice(stable.slots() + 1) : 0;

    // PetStable_Update. A warlock's pet out: PetStable_NoPetsAllowed (nothing shown, the current slot
    // disabled).
    const bool noPets = pet && !isHunterPet;
    const std::string_view emptySlot = text::get("EMPTY_STABLE_SLOT");
    const auto petIcon = [&](u32 entry) {
        return m_pets ? m_pets->familyIconHash(familyOf(session, entry)) : 0u;
    };
    const u32 petEntry = pet ? pet->value(uf::OBJECT_FIELD_ENTRY) : 0;
    i32 selected = -1;
    if (!noPets) {
        selected = stable.autoSelect(isHunterPet); // "If no selected pet try to set one"
    }
    // The stabled slots.
    for (u32 i = 1; i <= 2; ++i) {
        StableSlotView& s = v.stabled[i - 1];
        s.purchased = i <= stable.slots();
        s.picked = stable.picked() == static_cast<i32>(i);
        if (noPets) {
            s.tooltip = emptySlot;
            continue;
        }
        const game::StabledPet* p = stable.pet(i);
        if (p) {
            s.iconHash = petIcon(p->entry);
            s.tooltip = p->nameView();
            s.tooltipSub = levelText(m_slotSub[i], {}, p->level, familyOf(session, p->entry));
        } else {
            s.tooltip = emptySlot;
        }
        s.checked = s.purchased && static_cast<i32>(i) == selected && p != nullptr;
    }
    // The current pet's slot: the pet out (GetPetIcon), else the first record (a dismissed or dead pet).
    StableSlotView& cur = v.current;
    cur.enabled = !noPets;
    cur.picked = stable.picked() == 0;
    const game::StabledPet* record = stable.pet(0);
    if (isHunterPet) {
        cur.iconHash = petIcon(petEntry);
        cur.tooltip = play.abilities().petName();
        cur.tooltipSub =
            levelText(m_slotSub[0], {}, pet->value(uf::UNIT_FIELD_LEVEL), familyOf(session, petEntry));
    } else if (!noPets && record) {
        cur.iconHash = petIcon(record->entry);
        cur.tooltip = record->nameView();
        cur.tooltipSub = levelText(m_slotSub[0], {}, record->level, familyOf(session, record->entry));
    } else {
        cur.tooltip = emptySlot;
    }
    cur.checked = selected == 0 && (isHunterPet || record);

    // The selected pet's texts, diet and model.
    u32 dietMask = 0;
    if (selected == 0 && isHunterPet) {
        const std::string_view name = play.abilities().petName();
        v.levelText =
            levelText(m_stableLevel, name, pet->value(uf::UNIT_FIELD_LEVEL), familyOf(session, petEntry));
        if (m_pets) {
            v.loyaltyText = m_pets->loyaltyName(pet->byteValue(uf::UNIT_FIELD_BYTES_1, 1)); // GetPetLoyalty
            dietMask = m_pets->familyFoodMask(familyOf(session, petEntry));
        }
        v.showModel = true;
    } else if (const game::StabledPet* p =
                   selected >= 0 && !noPets ? stable.pet(static_cast<u32>(selected)) : nullptr) {
        v.levelText = levelText(m_stableLevel, p->nameView(), p->level, familyOf(session, p->entry));
        if (m_pets) {
            v.loyaltyText = m_pets->loyaltyName(p->loyalty);
            dietMask = m_pets->familyFoodMask(familyOf(session, p->entry));
        }
        v.showModel = true;
    }
    if (v.showModel && m_pets) {
        char foods[80];
        const std::string_view list = m_pets->dietList(dietMask, foods);
        v.dietText = text::format(m_stableDiet, "PET_DIET_TEMPLATE", list);
    }
    if (v.showModel && m_views) {
        if (const game::ActorLook* look = stableLook(session, selected, isHunterPet ? pet->guid : 0)) {
            v.model = m_views->show(ModelViewId::Stable, *look, m_stableModel.facing, kPetZoom);
        }
    }
    // The purchase: hidden with both slots bought (or a warlock's pet out); the cost red when unaffordable.
    v.showPurchase = !noPets && stable.slots() < game::PetStable::kStabledSlots;
    v.canPurchase = money >= v.nextSlotCost && v.nextSlotCost > 0;

    const StableResult r = stableWindow(c, v, m_stableModel);
    if (r.rotated) {
        m_uiSound(UiSound::InventoryRotate);
    }
    const bool petOut = pet != nullptr;
    if (r.pickup >= 0) {
        stable.pickup(r.pickup); // OnDragStart / the long press
    }
    if (r.drop >= 0) {
        stable.click(r.drop, petOut); // OnReceiveDrag
    } else if (r.click >= 0) {
        stable.click(r.click, petOut); // OnClick: select, or drop what the finger carries
    }
    if (r.purchase) {
        stable.buySlot(v.nextSlotCost, money); // BuyStableSlot
    }
    if (r.closed) {
        stable.close(); // the close button: HideUIPanel, then OnHide's ClosePetStables
    }
}

// --- The character window's Pet tab ------------------------------------------------------------------------

bool PetScreens::hasPetUI(const GameSession& session) {
    const game::PetState& pet = session.play().abilities().pet();
    const Entity* e = pet.active() ? session.entities().find(pet.guid) : nullptr;
    return e && (e->type != game::ObjectType::Player) && e->value(uf::UNIT_FIELD_PETNUMBER) != 0;
}

void PetScreens::petTab(GameSession& session, DialogContext& c, const ui::Rect& body) {
    VD_PROFILE_ZONE("PetScreens::petTab");
    const Entity* pet = petEntity(session);
    if (!pet || !hasPetUI(session)) {
        return; // PetPaperDollFrame_Update: nothing without a pet (the tab hides)
    }
    game::Gameplay& play = session.play();
    PetPaperDollView& v = m_paperDollView;
    v = PetPaperDollView{};
    const bool hunter = hunterPet(session, pet);
    v.hunter = hunter;
    const u32 entry = pet->value(uf::OBJECT_FIELD_ENTRY);
    const u32 family = familyOf(session, entry);
    const u32 level = pet->value(uf::UNIT_FIELD_LEVEL);
    v.name = play.abilities().petName();
    if (v.name.empty()) {
        if (const net::CreatureQueryInfo* info = session.creature(entry)) {
            std::snprintf(m_name.data(), m_name.size(), "%s", info->name.c_str());
            v.name = m_name.data();
        }
    }
    if (family != 0) { // PetLevelText only when UnitCreatureFamily("pet") answers
        v.levelText = levelText(m_level, {}, level, family);
    }
    if (hunter && m_pets) {
        v.loyaltyText = m_pets->loyaltyName(pet->byteValue(uf::UNIT_FIELD_BYTES_1, 1));
        char foods[80];
        const std::string_view list = m_pets->dietList(m_pets->familyFoodMask(family), foods);
        v.dietText = text::format(m_diet, "PET_DIET_TEMPLATE", list);
        // GetPetTrainingPoints: the field's high half minus its low half (vMaNGOS' Pet::GetDispTP encoding).
        const u32 tp = pet->value(uf::UNIT_TRAINING_POINTS);
        std::snprintf(m_training.data(), m_training.size(), "%d",
                      static_cast<i32>(tp >> 16) - static_cast<i32>(tp & 0xFFFFu));
        v.trainingText = m_training.data();
        v.xp = pet->value(uf::UNIT_FIELD_PETEXPERIENCE); // GetPetExperience (hunters only)
        v.nextXp = pet->value(uf::UNIT_FIELD_PETNEXTLEVELEXP);
    }

    // PetPaperDollFrame_SetStats: a pet has no buff fields (UNIT_FIELD_POSSTAT* are the player's): the stat
    // itself, white; the tooltip "Strength 20" and the class's text (<CLASS>_<STAT>_TOOLTIP, else
    // DEFAULT_<STAT>_TOOLTIP).
    static constexpr text::Key kStatNames[5] = {"SPELL_STAT0_NAME", "SPELL_STAT1_NAME", "SPELL_STAT2_NAME",
                                                "SPELL_STAT3_NAME", "SPELL_STAT4_NAME"};
    static constexpr const char* kStatTokens[5] = {"STRENGTH", "AGILITY", "STAMINA", "INTELLECT", "SPIRIT"};
    const std::string_view classFile =
        m_pets ? m_pets->classFileName(pet->byteValue(uf::UNIT_FIELD_BYTES_0, 1)) : std::string_view{};
    for (u32 i = 0; i < 5; ++i) {
        // "Strength:" (TEXT(getglobal("SPELL_STAT"..(i-1).."_NAME"))..":").
        const std::string_view name = text::get(kStatNames[i]);
        std::snprintf(m_statLabel[i].data(), m_statLabel[i].size(), "%.*s:", static_cast<int>(name.size()),
                      name.data());
        const i32 value = intField(*pet, static_cast<u16>(uf::UNIT_FIELD_STAT0 + i));
        std::snprintf(m_statValue[i].data(), m_statValue[i].size(), "%d", value);
        std::snprintf(m_statTip[i].data(), m_statTip[i].size(), "%.*s %d", static_cast<int>(name.size()),
                      name.data(), value);
        char key[48];
        std::snprintf(key, sizeof(key), "%.*s_%s_TOOLTIP", static_cast<int>(classFile.size()),
                      classFile.data(), kStatTokens[i]);
        if (classFile.empty() || !text::has(key)) {
            std::snprintf(key, sizeof(key), "DEFAULT_%s_TOOLTIP", kStatTokens[i]);
        }
        v.stats[i] = {m_statLabel[i].data(), m_statValue[i].data(), ui::kWhite, m_statTip[i].data(),
                      text::get(key)};
    }

    // Attack (UnitAttackBothHands: a creature's weapon skill, its level x 5), Power (UnitAttackPower with
    // UNIT_FIELD_ATTACK_POWER_MODS), Damage (UnitDamage: no bonus fields for a pet), Defense (its level x 5),
    // Armor.
    const u32 skill = level * 5;
    std::snprintf(m_combatValue[0].data(), m_combatValue[0].size(), "%u", skill);
    v.combat[0] = {
        text::get("ATTACK_COLON"), m_combatValue[0].data(), ui::kWhite, text::get("ATTACK_TOOLTIP"), {}};

    const i32 apBase = intField(*pet, uf::UNIT_FIELD_ATTACK_POWER);
    const u32 apMods = pet->value(uf::UNIT_FIELD_ATTACK_POWER_MODS);
    const i32 apPos = static_cast<i16>(apMods & 0xFFFF), apNeg = static_cast<i16>(apMods >> 16);
    const i32 ap = std::max(0, apBase + apPos + apNeg);
    std::snprintf(m_combatValue[1].data(), m_combatValue[1].size(), "%d", ap);
    {
        // PaperDollFormatStat(MELEE_ATTACK_POWER, base, posBuff, negBuff): "Melee Attack Power 120 (100+20)".
        const std::string_view name = text::get("MELEE_ATTACK_POWER");
        usize n = static_cast<usize>(std::snprintf(m_combatTip[1].data(), m_combatTip[1].size(), "%.*s %d",
                                                   static_cast<int>(name.size()), name.data(), ap));
        if ((apPos > 0 || apNeg < 0) && n < m_combatTip[1].size()) {
            n += static_cast<usize>(
                std::snprintf(m_combatTip[1].data() + n, m_combatTip[1].size() - n, " (%d", apBase));
            if (apPos > 0 && n < m_combatTip[1].size()) {
                n += static_cast<usize>(std::snprintf(m_combatTip[1].data() + n, m_combatTip[1].size() - n,
                                                      "|cff20ff20+%d|r", apPos));
            }
            if (apNeg < 0 && n < m_combatTip[1].size()) {
                n += static_cast<usize>(std::snprintf(m_combatTip[1].data() + n, m_combatTip[1].size() - n,
                                                      " |cffff2020%d|r", apNeg));
            }
            if (n < m_combatTip[1].size()) {
                std::snprintf(m_combatTip[1].data() + n, m_combatTip[1].size() - n, ")");
            }
        }
        text::format(m_combatSub[0], "MELEE_ATTACK_POWER_TOOLTIP",
                     static_cast<f32>(ap) / 14.0f); // ATTACK_POWER_MAGIC_NUMBER
    }
    v.combat[1] = {text::get("ATTACK_POWER_COLON"), m_combatValue[1].data(), valueColor(apPos, apNeg),
                   m_combatTip[1].data(), m_combatSub[0].data()};

    const f32 minDamage = floatField(*pet, uf::UNIT_FIELD_MINDAMAGE),
              maxDamage = floatField(*pet, uf::UNIT_FIELD_MAXDAMAGE);
    const i32 shownMin = std::max(static_cast<i32>(std::floor(minDamage)), 1);
    const i32 shownMax = std::max(static_cast<i32>(std::ceil(maxDamage)), 1);
    std::snprintf(m_combatValue[2].data(), m_combatValue[2].size(),
                  shownMin < 100 && shownMax < 100 ? "%d - %d" : "%d-%d", shownMin, shownMax);
    {
        // CharacterDamageFrame_OnEnter: INVTYPE_WEAPONMAINHAND, the speed, the damage and the damage per
        // second.
        const f32 speed =
            std::max(0.001f, static_cast<f32>(pet->value(uf::UNIT_FIELD_BASEATTACKTIME)) / 1000.0f);
        const f32 dps = std::max((minDamage + maxDamage) * 0.5f, 1.0f) / speed;
        const std::string_view speedLabel = text::get("ATTACK_SPEED_COLON");
        const std::string_view damageLabel = text::get("DAMAGE_COLON");
        const std::string_view dpsLabel = text::get("DAMAGE_PER_SECOND");
        std::snprintf(m_combatSub[1].data(), m_combatSub[1].size(), "%.*s %.2f\n%.*s %d - %d\n%.*s %.1f",
                      static_cast<int>(speedLabel.size()), speedLabel.data(), speed,
                      static_cast<int>(damageLabel.size()), damageLabel.data(), shownMin, shownMax,
                      static_cast<int>(dpsLabel.size()), dpsLabel.data(), dps);
    }
    v.combat[2] = {text::get("DAMAGE_COLON"), m_combatValue[2].data(), ui::kWhite,
                   text::get("INVTYPE_WEAPONMAINHAND"), m_combatSub[1].data()};

    std::snprintf(m_combatValue[3].data(), m_combatValue[3].size(), "%u", skill);
    v.combat[3] = {
        text::get("DEFENSE_COLON"), m_combatValue[3].data(), ui::kWhite, text::get("DEFENSE_TOOLTIP"), {}};

    const i32 armor = std::max(0, intField(*pet, uf::UNIT_FIELD_RESISTANCES));
    std::snprintf(m_combatValue[4].data(), m_combatValue[4].size(), "%d", armor);
    {
        const std::string_view name = text::get("ARMOR");
        std::snprintf(m_combatTip[4].data(), m_combatTip[4].size(), "%.*s %d", static_cast<int>(name.size()),
                      name.data(), armor);
        // PaperDollFrame_SetArmor: the reduction against an attacker of its level.
        const f32 r = static_cast<f32>(armor) / (85.0f * static_cast<f32>(std::max(level, 1u)) + 400.0f);
        text::format(m_armorSub, "ARMOR_TOOLTIP", level, 100.0f * (r / (r + 1.0f)));
        v.combat[4] = {text::get("ARMOR_COLON"), m_combatValue[4].data(), ui::kWhite, m_combatTip[4].data(),
                       m_armorSub.data()};
    }

    // PetPaperDollFrame_SetResistances: arcane, fire, nature, frost, shadow; red negative, green positive.
    static constexpr u16 kSchools[5] = {6, 2, 3, 4, 5};
    static constexpr text::Key kResistNames[5] = {"RESISTANCE6_NAME", "RESISTANCE2_NAME", "RESISTANCE3_NAME",
                                                  "RESISTANCE4_NAME", "RESISTANCE5_NAME"};
    for (u32 i = 0; i < 5; ++i) {
        const i32 value = intField(*pet, static_cast<u16>(uf::UNIT_FIELD_RESISTANCES + kSchools[i]));
        std::snprintf(m_resistValue[i].data(), m_resistValue[i].size(), "%d", value);
        v.resistances[i] = {m_resistValue[i].data(),
                            value < 0   ? ui::kRed
                            : value > 0 ? ui::kGreen
                                        : ui::kWhite,
                            text::get(kResistNames[i])};
    }

    // The model: the pet as it stands in the world.
    if (m_views && m_portraits) {
        if (const game::ActorLook* look = m_portraits->look(pet->guid)) {
            v.model = m_views->show(ModelViewId::PetPaperDoll, *look, m_paperDoll.model.facing, kPetZoom);
        }
    }
    const PetPaperDollResult r = petPaperDoll(c, body, v, m_paperDoll);
    if (r.rotated) {
        m_uiSound(UiSound::InventoryRotate);
    }
}

// --- Over the HUD ------------------------------------------------------------------------------------------

void PetScreens::drawHud(GameSession& session, const Hud& hud, DialogContext& c, const ui::Rect& petFrame,
                         u32 nowMs) {
    VD_PROFILE_ZONE("PetScreens::drawHud");
    m_nowMs = nowMs;
    if (!session.inWorld()) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& player = hud.playerFrameRect();
    // PlayerFrame_UpdateGroupIndicator: in a raid, the player's subgroup (GetRaidRosterInfo's, 1..8).
    const game::GroupState& group = session.play().social().group();
    if (group.active && group.raid && player.w > 0.0f) {
        groupIndicator(g, player, static_cast<u32>(group.ownSubgroup) + 1);
    }
    // PlayerHitIndicator: centered at (73, 42) of the player frame; PetHitIndicator at (28, 27) of the pet
    // frame.
    if (player.w > 0.0f) {
        const f32 s = player.w / 232.0f;
        hitIndicator(g, player.x + 73.0f * s, player.y + 42.0f * s, s, m_playerFeedback.view(nowMs));
    }
    if (petFrame.w > 0.0f) {
        const f32 s = petFrame.w / 128.0f;
        hitIndicator(g, petFrame.x + 28.0f * s, petFrame.y + 27.0f * s, s, m_petFeedback.view(nowMs));
    }
}

} // namespace vd
