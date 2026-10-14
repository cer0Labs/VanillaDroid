#include "app/CharacterGallery.h"

#include "app/DialogKit.h"
#include "app/ModelFrameGallery.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterSheet.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;

constexpr u64 iconHash(const char* name) {
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}
constexpr u64 slotArt(const char* name) {
    u64 hash = assetPathHash("Interface\\PaperDoll\\UI-PaperDoll-Slot-");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

// A level-12 human warrior's equipment: the 19 slots, then the ammo.
struct Sample {
    const char* icon;
    u32 count;
    u32 quality;
};
constexpr Sample kEquipment[20] = {
    {nullptr, 0, 0},
    {"INV_Jewelry_Necklace_04", 1, 2},
    {nullptr, 0, 0},
    {"INV_Shirt_05", 1, 1},
    {"INV_Chest_Chain_05", 1, 2},
    {"INV_Belt_01", 1, 0},
    {"INV_Pants_02", 1, 1},
    {"INV_Boots_05", 1, 1},
    {"INV_Bracer_02", 1, 2},
    {"INV_Gauntlets_02", 1, 1},
    {"INV_Jewelry_Ring_02", 1, 2},
    {nullptr, 0, 0},
    {nullptr, 0, 0},
    {nullptr, 0, 0},
    {"INV_Misc_Cape_02", 1, 2},
    {"INV_Sword_04", 1, 2},
    {"INV_Shield_04", 1, 1},
    {"INV_Weapon_Bow_02", 1, 1},
    {nullptr, 0, 0},
    {"INV_Ammo_Arrow_01", 343, 1},
};
constexpr const char* kSlotArtNames[20] = {"Head",     "Neck",          "Shoulder", "Shirt",   "Chest",
                                           "Waist",    "Legs",          "Feet",     "Wrists",  "Hands",
                                           "Finger",   "Finger",        "Trinket",  "Trinket", "Chest",
                                           "MainHand", "SecondaryHand", "Ranged",   "Tabard",  "Ammo"};

u32 floatBits(f32 f) {
    u32 b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

game::FactionInfo faction(u32 id, i32 listId, u32 parent, std::string_view name,
                          std::string_view description) {
    game::FactionInfo f;
    f.id = id;
    f.listId = listId;
    f.parent = parent;
    f.name = name;
    f.description = description;
    f.raceMask[0] = 0xFF;
    return f;
}

game::SkillLineSource skill(u32 id, std::string_view name, u32 category, u32 order,
                            std::string_view categoryName, u32 value, u32 max, std::string_view description,
                            u32 flags = 0x80) {
    game::SkillLineSource s;
    s.skill = id;
    s.name = name;
    s.description = description;
    s.category = category;
    s.categoryOrder = order;
    s.categoryName = categoryName;
    s.value = value;
    s.max = max;
    s.flags = flags;
    return s;
}

// The sample weapons' templates (the sword's skill, the bow's school and speed).
net::ItemQueryInfo weaponTemplate(u32 subclass) {
    net::ItemQueryInfo t;
    t.found = true;
    t.itemClass = 2;
    t.subclass = subclass;
    t.damages[0].min = 5.0f;
    t.damages[0].max = 10.0f;
    return t;
}

} // namespace

void CharacterGallery::build() {
    m_built = true;
    // The player's update fields as vMaNGOS sends them: buffed strength, a debuffed spirit, armor with a
    // buff, a fire resistance potion, a sword skill with a temporary bonus, honor kills of a week of
    // battlegrounds.
    m_player.guid = 0x11;
    m_player.type = game::ObjectType::Player;
    m_player.values.assign(uf::PLAYER_END, 0);
    const auto set = [&](u16 index, u32 value) { m_player.values[index] = value; };
    set(uf::UNIT_FIELD_BYTES_0, 1u | (1u << 8)); // human warrior, male
    set(uf::UNIT_FIELD_LEVEL, 12);
    const u32 stats[5] = {52, 31, 40, 20, 27};
    for (u16 i = 0; i < 5; ++i) {
        set(static_cast<u16>(uf::UNIT_FIELD_STAT0 + i), stats[i]);
    }
    set(uf::PLAYER_FIELD_POSSTAT0, 8);                        // Battle Shout? a Strength buff
    set(uf::PLAYER_FIELD_NEGSTAT0 + 4, static_cast<u32>(-3)); // a curse on spirit
    set(uf::UNIT_FIELD_RESISTANCES, 612);
    set(uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE, 30);
    set(uf::UNIT_FIELD_RESISTANCES + 2, 10);
    set(uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE + 2, 10);
    set(uf::UNIT_FIELD_ATTACK_POWER, 110);
    set(uf::UNIT_FIELD_ATTACK_POWER_MODS, 20);
    set(uf::UNIT_FIELD_RANGED_ATTACK_POWER, 31);
    set(uf::UNIT_FIELD_BASEATTACKTIME, 2600);
    set(uf::UNIT_FIELD_RANGEDATTACKTIME, 2800);
    set(uf::UNIT_FIELD_MINDAMAGE, floatBits(19.6f));
    set(uf::UNIT_FIELD_MAXDAMAGE, floatBits(33.2f));
    set(uf::UNIT_FIELD_MINRANGEDDAMAGE, floatBits(7.1f));
    set(uf::UNIT_FIELD_MAXRANGEDDAMAGE, floatBits(13.9f));
    for (u16 school = 0; school < 7; ++school) {
        set(static_cast<u16>(uf::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT + school), floatBits(1.0f));
    }
    const u32 weaponSkills[3][3] = {
        {43, 58u | (60u << 16), 2}, {45, 35u | (60u << 16), 0}, {162, 12u | (60u << 16), 0}};
    for (u16 i = 0; i < 3; ++i) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + i * 3);
        set(base, weaponSkills[i][0]);
        set(static_cast<u16>(base + 1), weaponSkills[i][1]);
        set(static_cast<u16>(base + 2), weaponSkills[i][2]);
    }
    set(uf::PLAYER_FIELD_SESSION_KILLS, 3u | (1u << 16));
    set(uf::PLAYER_FIELD_YESTERDAY_KILLS, 12);
    set(uf::PLAYER_FIELD_YESTERDAY_CONTRIBUTION, 246);
    set(uf::PLAYER_FIELD_THIS_WEEK_KILLS, 27);
    set(uf::PLAYER_FIELD_THIS_WEEK_CONTRIBUTION, 812);
    set(uf::PLAYER_FIELD_LAST_WEEK_KILLS, 64);
    set(uf::PLAYER_FIELD_LAST_WEEK_CONTRIBUTION, 2934);
    set(uf::PLAYER_FIELD_LAST_WEEK_RANK, 1534);
    set(uf::PLAYER_FIELD_LIFETIME_HONORBALE_KILLS, 391);
    set(uf::PLAYER_FIELD_LIFETIME_DISHONORBALE_KILLS, 2);
    set(uf::PLAYER_FIELD_BYTES, 7u << 24); // highest rank: Sergeant
    set(uf::PLAYER_BYTES_3, 6u << 24);     // rank: Corporal
    set(uf::PLAYER_FIELD_BYTES2, 140);

    m_equipment.clear();
    for (u8 i = 0; i < 20; ++i) {
        ItemSlotView cell;
        cell.ref = {ItemRef::Kind::Slot, 255, i < 19 ? i : u8{200}, 0};
        cell.emptyArt = slotArt(kSlotArtNames[i]);
        if (kEquipment[i].icon) {
            cell.empty = false;
            cell.iconHash = iconHash(kEquipment[i].icon);
            cell.count = i == 19 ? kEquipment[i].count : 0;
            cell.quality = kEquipment[i].quality;
        }
        m_equipment.push_back(cell);
    }
    m_equipment[15].lowDurability = true;

    // Reputation: a human's list (Faction.dbc's names and parents; placeholder descriptions, sample standings).
    m_factionInfo = {
        faction(469, 11, 0, "Alliance", {}),
        faction(72, 19, 469, "Stormwind",
                "The great human city of the Alliance, behind its white walls and canals.  Its guards, "
                "merchants and nobles remember who helped them."),
        faction(47, 20, 469, "Ironforge",
                "The dwarven city carved into the mountain of Dun Morogh.  Forges never cool here."),
        faction(69, 21, 469, "Darnassus",
                "The night elves' city among the branches of Teldrassil, far across the sea to the "
                "west.  Its sentinels keep a close watch on strangers who come by boat."),
        faction(54, 18, 469, "Gnomeregan Exiles",
                "Gnomes who lost their city under Dun Morogh and now live as guests of the dwarves, "
                "still loyal to the Alliance."),
        faction(169, 10, 0, "Steamwheedle Cartel", {}),
        faction(21, 1, 169, "Booty Bay",
                "A busy goblin port at the southern tip of Stranglethorn, where any coin is welcome.  "
                "The pirates of the Bloodsail fleet are no friends of its harbor."),
        faction(369, 7, 169, "Gadgetzan",
                "A goblin trading town in the desert of Tanaris, with good engineers and better prices."),
        faction(529, 13, 0, "Argent Dawn",
                "An order sworn to fight the undead wherever they rise.  Its camps stand in the "
                "Plaguelands, east and west, and it rewards those who help."),
        faction(70, 6, 0, "Syndicate",
                "A band of thieves and smugglers hiding in the hills of Alterac and around Hillsbrad."),
        faction(609, 36, 0, "Cenarion Circle",
                "Druids who watch over the wild places, gathered in the quiet glades of Moonglade."),
    };
    net::InitializeFactions init;
    init.count = net::kFactionListSize;
    init.factions[11] = {0x09, 0};
    init.factions[19] = {0x11, 16440}; // Honored 7440 / 12000 (the watched one)
    init.factions[20] = {0x11, 3200};
    init.factions[21] = {0x11, 0};
    init.factions[18] = {0x11, 2900};
    init.factions[10] = {0x08, 0};
    init.factions[1] = {0x01, -2900};
    init.factions[7] = {0x01, 1200};
    init.factions[13] = {0x01, 600};
    init.factions[6] = {0x03, -10000}; // at war
    init.factions[36] = {0x21, 0};     // inactive
    m_reputation.onInitialize(init);
    m_factions.select(-1);

    // Skills (SkillLine.dbc's names and the categories' sort order; placeholder descriptions).
    const std::string_view weapon = "More skill means your attacks with it hit more often.";
    m_skillSources = {
        skill(26, "Arms", 7, 2, "Class Skills", 1, 1, {}, 0x410),
        skill(257, "Protection", 7, 2, "Class Skills", 1, 1, {}, 0x410),
        skill(186, "Mining", 11, 3, "Professions", 47, 75,
              "The more you mine, the harder the ore you can dig up.  A vein that resists you only "
              "means you need more practice: go back to easier veins in the gentler lands and "
              "come back later.",
              0xA0),
        skill(129, "First Aid", 9, 4, "Secondary Skills", 30, 75,
              "More skill opens better bandages and treatments.  Trainers teach them, and some "
              "come as quest rewards or from the bodies of fallen foes."),
        skill(185, "Cooking", 9, 4, "Secondary Skills", 1, 75,
              "More skill opens tastier dishes.  Recipes come from trainers and vendors, from quests and "
              "now and then from fallen foes."),
        skill(43, "Swords", 6, 5, "Weapon Skills", 58, 60, weapon),
        skill(45, "Bows", 6, 5, "Weapon Skills", 35, 60, weapon),
        skill(162, "Unarmed", 6, 5, "Weapon Skills", 12, 60, "More skill: your fists hit more often."),
        skill(95, "Defense", 6, 5, "Weapon Skills", 60, 60,
              "More defense: enemies miss you more often and rarely land a crushing blow on you."),
        skill(413, "Mail", 8, 6, "Armor Proficiencies", 1, 1, "Lets you wear mail armor."),
        skill(414, "Leather", 8, 6, "Armor Proficiencies", 1, 1, "Lets you wear leather armor."),
        skill(433, "Shield", 8, 6, "Armor Proficiencies", 1, 1, "Lets you use shields."),
        skill(98, "Language: Common", 10, 7, "Languages", 300, 300, {}),
    };
    m_skillSources[2].step = 1;      // Apprentice Miner: the Unlearn button
    m_skillSources[5].tempBonus = 2; // a sharpening stone
    m_skills.build(m_skillSources, 12);
    m_skills.select(3); // Mining
}

void CharacterGallery::draw(DialogContext& c, Page page) {
    VD_PROFILE_ZONE("CharacterGallery::draw");
    if (!m_built) {
        build();
    }
    if (page != m_page) {
        m_page = page;
        m_tab = page == Page::Reputation ? 2 : page == Page::Skills ? 3 : page == Page::Honor ? 4 : 0;
        m_inspectTab = page == Page::InspectHonor ? 1 : 0;
    }
    paperDoll(c, page == Page::Inspect || page == Page::InspectHonor, m_tab);
}

void CharacterGallery::paperDoll(DialogContext& c, bool inspect, u32 tab) {
    (void)tab;
    PaperDollView view;
    view.equipment = m_equipment;
    char level[96];
    if (inspect) {
        // InspectFrame: the slots and the model, the HONOR tab (no numbers, no ammo slot).
        const std::string_view tabs[2] = {text::get("CHARACTER"), text::get("HONOR")};
        view.title = "Grukka";
        view.level = text::format(level, "PLAYER_LEVEL", 14, "Orc", "Warrior");
        view.tabs = tabs;
        view.tab = m_inspectTab;
        view.page = m_inspectTab == 1 ? CharacterPage::Honor : CharacterPage::PaperDoll;
        view.ammoSlot = false;
        view.modelState = &m_inspectModel;
        view.model = m_models ? m_models->sample(ModelViewId::Inspect, ModelFrameGallery::Sample::Orc,
                                                 m_inspectModel.facing)
                              : TextureHandle{};
        ItemDrag none;
        const PaperDollResult r = paperDollWindow(c, view, none);
        if (r.items.tab >= 0) {
            m_inspectTab = static_cast<u32>(r.items.tab);
        } else if (m_inspectTab == 1) {
            honor(c, r.frame, false); // InspectHonorFrame: no level line
        }
        if (r.items.closed) {
            logResult("inspect closed");
        }
        return;
    }
    const std::string_view tabs[4] = {text::get("CHARACTER"), text::get("REPUTATION"), text::get("SKILLS"),
                                      text::get("HONOR")};
    // CharacterFrame's tabs as the game shows them without a pet: CHARACTER, REPUTATION, SKILLS, HONOR.
    static constexpr u32 kTabIds[4] = {0, 2, 3, 4};
    u32 shown = 0;
    for (u32 i = 0; i < 4; ++i) {
        shown = kTabIds[i] == m_tab ? i : shown;
    }
    char title[96];
    char rankName[64];
    const std::string_view rankKey = game::pvpRankKey(rankName, 6, true);
    view.title = text::format(title, "UNIT_PVP_NAME", text::get(text::Key(rankKey)), "Arthel");
    view.level = text::format(level, "PLAYER_LEVEL", 12, "Human", "Warrior");
    char guild[96];
    view.guild = text::format(guild, "GUILD_TITLE_TEMPLATE", "Veteran", "Knights of Elwynn");
    view.tabs = tabs;
    view.tab = shown;
    view.page = static_cast<CharacterPage>(m_tab);
    game::PaperDollInput in;
    in.classFile = "WARRIOR";
    static const net::ItemQueryInfo kSword = weaponTemplate(7), kBow = weaponTemplate(2);
    in.mainHand = &kSword;
    in.ranged = &kBow;
    in.rangedEquipped = true;
    m_doll.build(m_player, in);
    for (u32 i = 0; i < game::kPaperDollStats; ++i) {
        const game::PaperDollLine& line = m_doll.line(static_cast<game::PaperDollStat>(i));
        m_stats[i] = {line.label, line.value, {line.tooltip.data(), line.tooltipLines}};
    }
    view.stats = m_stats;
    view.statShown = m_statShown;
    view.modelState = &m_model;
    view.model =
        m_models ? m_models->sample(ModelViewId::Character, ModelFrameGallery::Sample::Human, m_model.facing)
                 : TextureHandle{};
    const PaperDollResult r = paperDollWindow(c, view, m_drag);
    m_statShown = r.statTapped;
    if (r.items.tab >= 0) {
        m_tab = kTabIds[std::min<u32>(static_cast<u32>(r.items.tab), 3)];
    } else if (m_tab != 0) {
        tabBody(c, m_tab, r.frame);
    }
    char note[96];
    if (r.items.tapped.valid()) {
        std::snprintf(note, sizeof(note), "character slot %u tapped", r.items.tapped.slot);
        logResult(note);
    }
    if (r.items.closed) {
        logResult("character closed");
    }
    m_drag.track(c.ui.touches());
    m_drag.finish();
}

void CharacterGallery::tabBody(DialogContext& c, u32 tab, const CharacterPageFrame& frame) {
    char note[96];
    if (tab == 2) { // ReputationFrame
        game::FactionList::Source source;
        source.byList = [](const void* context, u32 listId) -> const game::FactionInfo* {
            for (const game::FactionInfo& f : *static_cast<const std::vector<game::FactionInfo>*>(context)) {
                if (f.listId == static_cast<i32>(listId)) {
                    return &f;
                }
            }
            return nullptr;
        };
        source.byId = [](const void* context, u32 id) -> const game::FactionInfo* {
            for (const game::FactionInfo& f : *static_cast<const std::vector<game::FactionInfo>*>(context)) {
                if (f.id == id) {
                    return &f;
                }
            }
            return nullptr;
        };
        source.context = &m_factionInfo;
        m_factions.build(m_reputation, source, 1, 1, 19);
        const std::span<const game::FactionRow> rows = m_factions.rows();
        if (m_factions.selectedFaction() == 0x7FFFFFFF && rows.size() > 2) {
            m_factions.select(2); // Stormwind
        }
        const i32 selected = m_factions.selected();
        u32 count = 0;
        for (const game::FactionRow& row : rows) {
            if (count == m_factionViews.size()) {
                break;
            }
            FactionRowView& v = m_factionViews[count];
            v = FactionRowView{};
            v.header = row.header;
            v.collapsed = row.collapsed;
            v.name = row.faction == game::kFactionOther            ? text::get("FACTION_OTHER")
                     : row.faction == game::kFactionInactiveHeader ? text::get("FACTION_INACTIVE")
                                                                   : row.name;
            static constexpr text::Key kLabels[8] = {"FACTION_STANDING_LABEL1", "FACTION_STANDING_LABEL2",
                                                     "FACTION_STANDING_LABEL3", "FACTION_STANDING_LABEL4",
                                                     "FACTION_STANDING_LABEL5", "FACTION_STANDING_LABEL6",
                                                     "FACTION_STANDING_LABEL7", "FACTION_STANDING_LABEL8"};
            v.standingId = row.standing;
            v.standing = text::get(kLabels[std::clamp<u32>(row.standing, 1, 8) - 1]);
            v.max = std::max(1, row.barMax - row.barMin);
            v.value = row.barValue - row.barMin;
            v.atWar = row.atWar;
            v.watched = row.watched;
            v.selected = static_cast<i32>(count) == selected;
            ++count;
        }
        FactionDetailView detail;
        if (m_factionDetail && selected >= 0) {
            const game::FactionRow& row = rows[static_cast<u32>(selected)];
            detail = {true,         row.name,   row.description, row.atWar, row.canToggleAtWar,
                      row.inactive, row.watched};
        }
        const ReputationPanelResult r =
            reputationPanel(c, frame, {m_factionViews.data(), count}, detail, m_reputationTip);
        if (r.headerTapped >= 0) {
            if (rows[static_cast<u32>(r.headerTapped)].collapsed) {
                m_factions.expand(r.headerTapped);
            } else {
                m_factions.collapse(r.headerTapped);
            }
        }
        if (r.rowTapped >= 0) {
            if (m_factionDetail && r.rowTapped == selected) {
                m_factionDetail = false;
            } else {
                m_factions.select(r.rowTapped);
                m_factionDetail = true;
            }
        }
        m_factionDetail = m_factionDetail && !r.closeDetail;
        if (r.toggleAtWar || r.toggleInactive || r.toggleWatched) {
            std::snprintf(note, sizeof(note), "reputation: war %d inactive %d watched %d",
                          r.toggleAtWar ? 1 : 0, r.toggleInactive ? 1 : 0, r.toggleWatched ? 1 : 0);
            logResult(note);
        }
        return;
    }
    if (tab == 3) { // SkillFrame
        m_skills.build(m_skillSources, 12);
        const std::span<const game::SkillLineRow> rows = m_skills.rows();
        const i32 selected = m_skills.selected();
        u32 count = 0;
        for (const game::SkillLineRow& row : rows) {
            if (count == m_skillViews.size()) {
                break;
            }
            SkillLineView& v = m_skillViews[count];
            v = SkillLineView{};
            v.header = row.header;
            v.expanded = row.expanded;
            v.name = row.name;
            if (!row.header && row.maxRank > 1) {
                const int n =
                    row.modifier == 0
                        ? std::snprintf(m_skillRanks[count].data(), 40, "%u/%u", row.rank, row.maxRank)
                        : std::snprintf(m_skillRanks[count].data(), 40, "%u (|cff20ff20+%d|r)/%u", row.rank,
                                        row.modifier, row.maxRank);
                v.rank = {m_skillRanks[count].data(), static_cast<usize>(std::max(n, 0))};
            }
            v.fill = row.maxRank > 1 ? static_cast<f32>(row.rank) / static_cast<f32>(row.maxRank) : 1.0f;
            v.proficiency = row.maxRank == 1;
            v.selected = static_cast<i32>(count) == selected;
            ++count;
        }
        SkillDetailView detail;
        if (selected >= 0 && static_cast<u32>(selected) < rows.size() &&
            !rows[static_cast<u32>(selected)].header) {
            const game::SkillLineRow& row = rows[static_cast<u32>(selected)];
            detail.shown = true;
            detail.name = row.name;
            detail.rank = m_skillViews[static_cast<u32>(selected)].rank;
            detail.fill = m_skillViews[static_cast<u32>(selected)].fill;
            detail.proficiency = row.maxRank == 1;
            detail.abandonable = row.abandonable;
            detail.description = text::format(m_skillDescription, "SKILL_DESCRIPTION", "", row.description);
        }
        const SkillPanelResult r =
            skillPanel(c, frame, {m_skillViews.data(), count}, m_skills.allExpanded(), detail, m_skillTip);
        if (r.close) {
            logResult("skills closed");
        }
        if (r.headerTapped >= 0) {
            if (rows[static_cast<u32>(r.headerTapped)].expanded) {
                m_skills.collapse(r.headerTapped);
            } else {
                m_skills.expand(r.headerTapped);
            }
        }
        if (r.allTapped) {
            if (m_skills.allExpanded()) {
                m_skills.collapse(-1);
            } else {
                m_skills.expand(-1);
            }
        }
        if (r.rowTapped >= 0) {
            m_skills.select(r.rowTapped);
        }
        if (r.unlearn) {
            logResult("skills: unlearn (UNLEARN_SKILL asks)");
        }
        return;
    }
    if (tab == 4) { // HonorFrame
        honor(c, frame, true);
    }
}

void CharacterGallery::honor(DialogContext& c, const CharacterPageFrame& frame, bool withLevel) {
    HonorView honor;
    char level[96], title[64], number[32], highest[64], path[64];
    if (withLevel) {
        honor.level = text::format(level, "PLAYER_LEVEL", 12, "Human", "Warrior");
    }
    const game::HonorStats h = game::honorStats(m_player);
    const std::string_view rankKey = game::pvpRankKey(title, h.rank, h.alliance);
    honor.rankTitle = rankKey.empty() ? text::get("NONE") : text::get(text::Key(rankKey));
    const i32 n = game::pvpRankNumber(h.rank);
    honor.rankNumber = text::format(number, "VD_CHAR_RANK_NUMBER", text::get("RANK"), n);
    std::snprintf(path, sizeof(path), "Interface\\PvPRankBadges\\PvPRank%02d.blp", n);
    honor.rankIcon = n > 0 ? assetPathHash(path) : 0;
    honor.progress = h.progress;
    honor.alliance = h.alliance;
    honor.todayHonorable = h.todayHonorable;
    honor.todayDishonorable = h.todayDishonorable;
    honor.yesterdayHonorable = h.yesterdayHonorable;
    honor.yesterdayHonor = h.yesterdayHonor;
    honor.thisWeekHonorable = h.thisWeekHonorable;
    honor.thisWeekHonor = h.thisWeekHonor;
    honor.lastWeekHonorable = h.lastWeekHonorable;
    honor.lastWeekHonor = h.lastWeekHonor;
    honor.lastWeekStanding = h.lastWeekStanding;
    honor.lifetimeHonorable = h.lifetimeHonorable;
    honor.lifetimeDishonorable = h.lifetimeDishonorable;
    const std::string_view highKey = game::pvpRankKey(highest, h.highestRank, h.alliance);
    honor.highestRank = highKey.empty() ? text::get("NONE") : text::get(text::Key(highKey));
    honorPanel(c, frame, honor, m_honorTip);
}

} // namespace vd
