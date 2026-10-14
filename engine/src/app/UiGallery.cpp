#include "app/UiGallery.h"

#include "app/AppOnlyGallery.h" // phase 22 (apppack)
#include "app/DressUpScreens.h"
#include "app/MicroMenu.h"
#include "app/MinimapView.h"
#include "app/UiPanels.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/Travel.h"
#include "game/WorldMapData.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::widgetId;

constexpr const char* kPageNames[] = {"gossip", "details", "progress", "reward",  "log",     "loot",
                                      "chat",   "menu",    "settings", "confirm", "widgets", "login",
                                      "combat", "create",  "party",   "socialchat", "emotes", "sheet",  "social",
                                      "channels",
                                      // Phase 8 (app/InventoryGallery).
                                      "bags", "character", "tooltip", "vendor", "bank", "reading", "quantity",
                                      "durability", "inspect",
                                      // Phase 8 (travel).
                                      "worldmap", "continent", "taxi", "loading", "zonetext",
                                      // Phase 8 (app/ExchangeGallery).
                                      "rolls", "mail", "mailsend", "trade", "auction", "auctionbids",
                                      "auctionsell", "mailedit",
                                      // Parity (app/PortraitGallery).
                                      "portraits",
                                      // Parity (app/MessagesGallery).
                                      "messages", "messagelog",
                                      // Parity (app/UnitsGallery).
                                      "units", "unittips", "unitmenu", "unitmenu2", "unitmenu3",
                                      // Parity (app/WorldLabelsGallery).
                                      "worldlabels",
                                      // Parity (app/AuraGallery).
                                      "auras",
                                      // Parity (app/PopupGallery).
                                      "popups", "popups2", "popups3",
                                      // Phase 12: the instances' popups, RaidInfoFrame.
                                      "popups4", "raidinfo",
                                      // Phase 16 (raidframe): the raid tab (app/RaidGallery: raid0, 2, 3).
                                      "raid",
                                      // Phase 16 (alerts): app/RaidAlertGallery.
                                      "readycheck", "raidwarning", "meetingstone",
                                      // Phase 13 (app/TradeSkillGallery).
                                      "tradeskill", "craft", "beasttraining",
                                      // Phase 13 (spells): app/TargetingGallery.
                                      "targeting", "targeting2",
                                      // Phase 13 (quests): the watch frame and the quest timers ("log": the log).
                                      "questwatch",
                                      // Phase 15 (app/ModelFrameGallery).
                                      "modelframe", "world",
                                      // Phase 15 (pets): app/PetGallery.
                                      "petbook", "petbar", "partypets", "stable", "petpaperdoll", "feedback",
                                      // Phase 15 (items): app/ItemsGallery.
                                      "containers", "compare", "dressup", "auctiondressup", "masterloot",
                                      "masterloot2", "lootpopups", "bankbags",
                                      // Phase 15 (character): app/CharacterGallery ("character", "inspect" too).
                                      "reputation", "skills", "honor", "inspecthonor",
                                      // Phase 15 (guild, help): app/GuildGallery (its other pages by name).
                                      "guild",
                                      // Phase 15 (options): app/OptionsGallery.
                                      "macro", "macropopup", "uioptions", "uioptions2", "soundoptions", "combattext",
                                      // Phase 16 (pullouts): app/RaidPulloutGallery.
                                      "pullouts", "pullouts2",
                                      // Phase 17 (honor): app/PvpGallery (its other pages by name).
                                      "pvp",
                                      // Phase 17 (queue): app/BattlefieldGallery (its other pages by name).
                                      "battlefield",
                                      // Phase 17 (scores): app/WorldStateGallery (its other pages by name).
                                      "worldstate",
                                      // Phase 18 (glue): app/GlueGallery (its other pages by name).
                                      "glue",
                                      // Phase 18 (create).
                                      "create2",
                                      // Phase 18 (p3).
                                      "p3",
                                      // Phase 18 (extras).
                                      "languages",
                                      // retouches (windows): app/WindowsGallery (its other pages by name).
                                      "talents2",
                                      // Retouches (ui models): app/UiModelsGallery (its other pages by name).
                                      "cooldowns",
                                      // Phase 22 (apppack): app/AppOnlyGallery (Game leaves ui.vdpk out).
                                      "apponly"};

constexpr u64 icon(const char* name) {
    // Interface\Icons\<name>.blp in textures.vdpk.
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
static_assert(icon("INV_Misc_Bag_07") == assetPathHash("Interface\\Icons\\INV_Misc_Bag_07.blp"));

// Northshire Valley samples shaped as the server sends them (quest_template, quest_greeting; $N and $c resolved),
// with placeholder texts.
constexpr GossipOptionView kGossipOptions[] = {
    {"Let me see your wares.", 1},
    {"Train me.", 3},
    {"Make this inn your home.", 5},
    {"Show me where I can fly.", 2},
    {"What do you know of the Defias?", 0},
};
constexpr GossipQuestView kGossipQuests[] = {
    {"Brotherhood of Thieves", 4, false, false},
    {"Bounty on Garrick Padfoot", 5, true, false},
    {"A Threat Within", 1, true, true},
};
constexpr std::string_view kWillemGreeting = "Well met, warrior.  I ought to be walking the streets with "
                                             "the city watch, but half of the "
                                             "Stormwind guard is away fighting in distant lands.  So the "
                                             "abbey makes do with me: a badge, a list of bounties and "
                                             "sore feet...";

const ItemView kMilitiaChoices[] = {
    {icon("INV_Weapon_ShortBlade_05"), "Militia Dagger", 1, 1},
    {icon("INV_Hammer_05"), "Militia Hammer", 1, 1},
    {icon("INV_Sword_04"), "Militia Shortsword", 1, 1},
    {icon("INV_Hammer_09"), "Militia Warhammer", 1, 2, 0, false}, // a choice the player cannot use: red
};
const ItemView kGarrickChoices[] = {
    {icon("INV_Pants_02"), "Tapered Pants", 1, 1},
    {icon("INV_Shirt_05"), "Layered Tunic", 1, 1},
    {icon("INV_Misc_Cape_02"), "Ensign Cloak", 1, 2},
};
const ItemView kWolfMeat[] = {{icon("INV_Misc_Food_14"), "Tough Wolf Meat", 8, 1}};
const ItemView kBonus[] = {{icon("INV_Misc_Food_15"), "Tough Hunk of Bread", 5, 1},
                           {icon("INV_Shield_09"), "Militia Buckler", 1, 1, 0, false}}; // given, unusable: dark red

constexpr ObjectiveView kKoboldObjectives[] = {{"Kobold Vermin", 6, 10, true}};
constexpr ObjectiveView kWolfObjectives[] = {{"Tough Wolf Meat", 8, 8}};
constexpr ObjectiveView kBandanaObjectives[] = {{"Red Burlap Bandana", 3, 12}};
constexpr ObjectiveView kEchoObjectives[] = {{"Kobold Worker", 0, 10, true}};
constexpr ObjectiveView kGarrickObjectives[] = {{"Garrick's Head", 0, 1}};
constexpr ObjectiveView kThreatObjectives[] = {{"Speak with Marshal McBride", 0, 0}};

constexpr QuestLogEntryView kQuestLog[] = {
    {"Kobold Camp Cleanup",
     "Kobolds have dug in among the trees north of the abbey, Arthel, and they grow bolder every night.  "
     "Head into the woods and thin out the vermin you find there.  Each one you drive off is one raid less "
     "on our stores.",
     "Slay 10 Kobold Vermin in the woods, then see Marshal McBride.", 2, false, false, kKoboldObjectives},
    {"Wolves Across the Border",
     "Those timber wolves keep raiding my drying racks!  Still, nothing beats a wolf steak over the fire...  "
     "Bring me tough wolf meat and I'll trade you something worth carrying.$B$BYou'll get the meat from the "
     "timber wolves and young wolves that roam the fields around the abbey.",
     "Gather 8 pieces of Tough Wolf Meat for Eagan Peltskinner by the abbey gate.", 2, true, false,
     kWolfObjectives},
    {"Brotherhood of Thieves",
     "A band of cutpurses calling themselves the Defias Brotherhood has crept into the valley.  They camp "
     "past the river to the east.$B$BWhatever they are plotting, I want it stopped.  Bring me the red "
     "bandanas they wear, and a blade from the armory is yours.",
     "Collect 12 Red Burlap Bandanas for Deputy Willem in front of the abbey.", 4, false, false,
     kBandanaObjectives},
    {"Investigate Echo Ridge",
     "Arthel, the scouts are back, and the kobolds number more than we feared.  A crew of kobold workers "
     "has settled by the Echo Ridge Mine up north.$B$BClear them out.  There are ten at the very least.  "
     "Deal with them, look around for more, then tell me what you saw.",
     "Defeat 10 Kobold Workers at the mine and report to Marshal McBride.", 3, false, false, kEchoObjectives},
    {"Bounty on Garrick Padfoot",
     "Garrick Padfoot has robbed our farmers and traders for weeks, and now he hides in a shack by the "
     "vineyards, over the bridge east of the abbey.  Bring me his head and the bounty is "
     "yours!$B$BMind yourself, Arthel.  He keeps a pack of thugs close at hand, and they will not let a "
     "stranger near him easily.",
     "Defeat Garrick Padfoot and take his head to Deputy Willem at the abbey.", 5, false, false,
     kGarrickObjectives},
    {"A Threat Within",
     "Lace your boots tight, recruit: there is plenty for a strong arm to do around the "
     "abbey.$B$BAnd none of it involves a plough.",
     "Find Marshal McBride.", 1, true, false, kThreatObjectives},
    {"Skirmish at Echo Ridge",
     "What you found down there settles it: the Echo Ridge Mine must be cleansed.  Go back in and help "
     "drive the kobolds out for good.",
     "Defeat 12 Kobold Laborers, then go back to Marshal McBride at the abbey.",
     7,
     false,
     true,
     {}},
};

const LootItemView kLoot[] = {
    {{icon("INV_Misc_Candle_02"), "Large Candle", 1, 1}, 1},
    {{icon("INV_Fabric_Linen_01"), "Linen Cloth", 2, 1}, 2},
    {{icon("INV_Misc_Pelt_Wolf_Ruin_02"), "Ruined Pelt", 1, 0}, 3},
    {{icon("INV_Bracer_02"), "Wolfskin Bracers", 1, 2}, 4}, // phase 8: a group roll (its note below)
};

constexpr text::Key kChannels[] = {"CHAT_MSG_SAY", "CHAT_MSG_YELL", "CHAT_MSG_PARTY", "CHAT_MSG_GUILD"};

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

} // namespace

bool UiGallery::init(UiResources* resources, u32 maxQuads) {
    const char* page = VD_DEV_ENV("VD_UI_GALLERY");
    if (!page || !*page || !resources || !resources->ready()) {
        return false;
    }
    if (std::strcmp(page, "all") == 0) {
        m_cycle = true;
    } else {
        u32 found = static_cast<u32>(Page::Count);
        for (u32 i = 0; i < std::size(kPageNames); ++i) {
            if (std::strcmp(page, kPageNames[i]) == 0) {
                found = i;
            }
        }
        if (found == static_cast<u32>(Page::Count) && m_combat.select(page)) {
            found = static_cast<u32>(Page::Combat);
        }
        if (found == static_cast<u32>(Page::Count) && m_guild.select(page)) { // phase 15 (guild, help)
            found = static_cast<u32>(Page::Guild);
        }
        if (found == static_cast<u32>(Page::Count) && m_raid.select(page)) { // phase 16 (raidframe)
            found = static_cast<u32>(Page::Raid);
        }
        if (found == static_cast<u32>(Page::Count) && m_pvp.select(page)) { // phase 17 (honor)
            found = static_cast<u32>(Page::Pvp);
        }
        if (found == static_cast<u32>(Page::Count) && m_battlefield.select(page)) { // phase 17 (queue)
            found = static_cast<u32>(Page::Battlefield);
        } else if (found == static_cast<u32>(Page::Battlefield)) {
            m_battlefield.select(page);
        }
        if (found == static_cast<u32>(Page::Count) && m_worldStates.select(page)) { // phase 17 (scores)
            found = static_cast<u32>(Page::WorldState);
        }
        // Phase 18 (glue).
        if (found == static_cast<u32>(Page::Count) && m_glue.select(page)) {
            found = static_cast<u32>(Page::Glue);
        }
        // Phase 18 (create).
        if ((found == static_cast<u32>(Page::Count) || found == static_cast<u32>(Page::CharCreate)) &&
            m_charCreate.select(page)) {
            found = static_cast<u32>(Page::CharCreate);
        }
        // Phase 18 (p3).
        if (found == static_cast<u32>(Page::Count) && m_p3.select(page)) {
            found = static_cast<u32>(Page::P3);
        }
        // Phase 18 (extras).
        if ((found == static_cast<u32>(Page::Count) || found == static_cast<u32>(Page::Extras)) &&
            m_extras.select(page)) {
            found = static_cast<u32>(Page::Extras);
        }
        // retouches (windows)
        if ((found == static_cast<u32>(Page::Count) || found == static_cast<u32>(Page::Windows)) &&
            m_windows.select(page)) {
            found = static_cast<u32>(Page::Windows);
        }
        // Retouches (ui models).
        if ((found == static_cast<u32>(Page::Count) || found == static_cast<u32>(Page::UiModels)) &&
            m_uiModels.select(page)) {
            found = static_cast<u32>(Page::UiModels);
        }
        if (found == static_cast<u32>(Page::Count)) {
            VD_LOG_WARN(
                "VD_UI_GALLERY=%s: no such page (gossip, details, progress, reward, log, loot, chat, menu, "
                "settings, confirm, widgets, login, combat, create, party, socialchat, emotes, sheet, social, "
                "channels, bags, character, tooltip, vendor, bank, reading, quantity, durability, worldmap, "
                "continent, taxi, loading, zonetext, rolls, mail, mailsend, trade, auction, auctionbids, "
                "auctionsell, mailedit, portraits, messages, messagelog, units, unittips, unitmenu, worldlabels, auras, all)",
                page);
            return false;
        }
        m_page = static_cast<Page>(found);
    }
    m_resources = resources;
    m_draw.init(&resources->assets(), maxQuads);
    m_options.init(nullptr, resources); // phase 15 (options): the macro icons
    if (const char* place = VD_DEV_ENV("VD_UI_GALLERY_PANEL")) {
        m_panelPlace = std::strcmp(place, "left") == 0    ? 1
                       : std::strcmp(place, "right") == 0 ? 2
                       : std::strcmp(place, "both") == 0  ? 3
                                                          : 0;
    }
    // The chat's sample: the kinds of lines a session shows (ChatTypeInfo colors).
    m_chatLines = {
        "|cffffff00Message of the day: have fun out there!|r",
        "|cffffff00Welcome to the Dev Realm server.|r",
        "|cffffc0c0[1. General - Elwynn Forest] [Tolvir]: anyone for Hogger? need 2 more|r",
        "[Arthel] says: Hello everyone",
        "[Marshal McBride] says: Hail, warrior.",
        "|cffff80ff[Aeryn] whispers: coming to the Echo Ridge mine?|r",
        "|cffff80ffTo [Aeryn]: on my way in a minute|r",
        "|cffaaaaff[Party] [Aeryn]: the kobolds at the entrance are mine|r",
        "|cff40ff40[Guild] [Brakk]: anyone got Linen Cloth? paying well|r",
        "|cffff4040[Hogger] yells: You smell like supper!|r",
        "|cffffff00You receive loot: |cffffffff[Linen Cloth]|rx2.|r",
        "|cffffff00You loot 7 Copper|r",
        "|cffffc0c0[2. Trade - City] [Galen]: WTS [Militia Shortsword] 1g, whisper me for a very long "
        "message that "
        "wraps over two lines at least on a phone|r",
        "|cff40ff40[Guild] [Arthel]: thanks!|r",
        "[Arthel] says: almost got the 10 tails",
    };
    m_chatViews.reserve(128);
    m_startMs = 0;
    VD_LOG_INFO("UI gallery: %s", m_cycle ? "all" : kPageNames[static_cast<u32>(m_page)]);
    return true;
}

void UiGallery::update(f32 width, f32 height, f32 dp, const ui::Rect& safeArea, u64 nowMs, ui::TextInput& input) {
    m_text = &input;
    VD_PROFILE_ZONE("UiGallery::update");
    m_draw.reset();
    if (!m_resources) {
        return;
    }
    if (m_startMs == 0) {
        m_startMs = nowMs;
    }
    m_ui.begin(width, height, dp, safeArea, nowMs);
    const ScreenProfile profile = width >= height * 1.6f ? ScreenProfile::Wide : ScreenProfile::Square;
    DialogContext c{m_ui, m_draw, *m_resources, profile, safeArea};
    c.playerLevel = 3;
    if (m_panelPlace != 0) {
        // The places as the game computes them, with the HUD's rectangles estimated (app/Hud's formulas): the frames'
        // 100 units, the action cluster in the corner, the micro menu, the minimap.
        const f32 unit = m_ui.px(ui::kUnitDp);
        const Rect& safe = safeArea;
        const f32 k = std::clamp(std::min(safe.h / m_ui.px(430.0f), safe.w * 0.5f / m_ui.px(466.0f)), 0.72f, 1.0f);
        const f32 big = m_ui.px(76.0f) * k, margin = m_ui.px(18.0f) * k;
        const f32 cx = safe.right() - margin - big * 0.5f, cy = safe.bottom() - margin - big * 0.5f - m_ui.px(4.0f) * k;
        UiPanelSpace space;
        space.safe = safe;
        space.dp = m_ui.dp();
        space.unit = unit;
        space.wide = profile == ScreenProfile::Wide;
        space.framesBottom = safe.y + 100.0f * unit;
        space.leftTop = safe.y + 160.0f * unit;
        const MinimapLayout map = minimapLayout(safe, unit, m_ui.dp());
        space.rightTop = map.top + 178.0f * map.s;
        space.cluster = {cx - m_ui.px(410.0f) * k, cy - m_ui.px(206.0f) * k, m_ui.px(410.0f) * k + big * 0.5f,
                         m_ui.px(206.0f) * k + big * 0.5f};
        const UiPanelPlaces places = uiPanelPlaces(space);
        m_draw.solid(space.cluster, rgba(255, 0, 0, 50));
        m_draw.solid(places.stickPad, rgba(0, 255, 0, 40));
        c.panel = m_panelPlace == 1 ? places.left : m_panelPlace == 2 && places.right.w > 0.0f ? places.right
                                                                                               : places.both;
        c.panelAlign = PanelAlign::TopLeft;
        c.profile = uiPanelWide(c.panel) ? ScreenProfile::Wide : ScreenProfile::Square;
        if (const char* shape = VD_DEV_ENV("VD_UI_GALLERY_PROFILE")) { // wide | square: compare the layouts
            c.profile = std::strcmp(shape, "wide") == 0 ? ScreenProfile::Wide : ScreenProfile::Square;
        }
    }
    drawPage(c);
    if (m_cycle) {
        // Over everything (dialogs block the rest of the screen).
        ui::Gui g{m_ui, m_draw};
        const Rect next{safeArea.x + g.px(8.0f), safeArea.y + g.px(6.0f), g.px(120.0f), g.px(36.0f)};
        if (ui::button(g, widgetId("gallery.next"), next, text::get("NEXT")) &&
            (m_page != Page::Combat || m_combat.next()) &&
            (m_page != Page::Guild || m_guild.next())) { // the combat (and guild) pages first, one by one
            m_page = static_cast<Page>((static_cast<u32>(m_page) + 1) % static_cast<u32>(Page::Count));
            VD_LOG_INFO("UI gallery: page %s", kPageNames[static_cast<u32>(m_page)]);
        }
    }
    m_ui.end();
}

void UiGallery::drawPage(DialogContext& c) {
    char line[96];
    switch (m_page) {
    case Page::Gossip: {
        const GossipView view{"Deputy Willem", kWillemGreeting, kGossipOptions, kGossipQuests};
        const GossipResult r = gossipDialog(c, view);
        if (r.option >= 0) {
            std::snprintf(line, sizeof(line), "gossip option %d", r.option);
            logResult(line);
        }
        if (r.quest >= 0) {
            std::snprintf(line, sizeof(line), "gossip quest %d", r.quest);
            logResult(line);
        }
        if (r.closed) {
            logResult("gossip closed");
        }
        break;
    }
    case Page::Details: {
        QuestDetailsView view;
        view.npcName = "Deputy Willem";
        view.title = "Brotherhood of Thieves";
        view.description = kQuestLog[2].description;
        view.objectives = kQuestLog[2].objectivesText;
        view.choices = kMilitiaChoices;
        view.rewards = kBonus;
        view.money = 150;
        view.xp = 355;
        const QuestDetailsResult r = questDetailsDialog(c, view);
        if (r.accept) {
            logResult("details accept");
        }
        if (r.decline) {
            logResult("details decline");
        }
        break;
    }
    case Page::Progress: {
        QuestProgressView view;
        view.npcName = "Eagan Peltskinner";
        view.title = "Wolves Across the Border";
        view.text = "Back already, Arthel?  My stomach's growling...got that tough wolf meat?";
        view.required = kWolfMeat;
        view.completable = true;
        const QuestProgressResult r = questProgressDialog(c, view);
        if (r.proceed) {
            logResult("progress proceed");
        }
        if (r.closed) {
            logResult("progress closed");
        }
        break;
    }
    case Page::Reward: {
        QuestRewardView view;
        view.npcName = "Deputy Willem";
        view.title = "Bounty on Garrick Padfoot";
        view.text = "Ha - so the rat is finally caught!  The whole valley owes you, and here's the bounty I "
                    "promised!";
        view.choices = kGarrickChoices;
        view.money = 75;
        view.xp = 335;
        const i32 before = m_rewardChoice;
        const QuestRewardResult r = questRewardDialog(c, view, m_rewardChoice);
        if (m_rewardChoice != before) {
            std::snprintf(line, sizeof(line), "reward selected %d", m_rewardChoice);
            logResult(line);
        }
        if (r.complete) {
            std::snprintf(line, sizeof(line), "reward complete choice %d", r.choice);
            logResult(line);
        }
        if (r.closed) {
            logResult("reward closed");
        }
        break;
    }
    case Page::Log: m_quests.drawLog(c, static_cast<u32>(m_ui.nowMs())); break; // phase 13: app/QuestGallery
    case Page::Loot: {
        std::array<LootItemView, std::size(kLoot)> items{};
        std::copy(std::begin(kLoot), std::end(kLoot), items.begin());
        items[3].note = text::get("VD_EXCHANGE_LOOT_ROLLING"); // phase 8: a group roll
        const LootView view{"Kobold Vermin", items, 7};
        const LootResult r = lootWindow(c, view);
        if (r.take >= 0) {
            std::snprintf(line, sizeof(line), "loot take %d (slot %u)", r.take, kLoot[r.take].slot);
            logResult(line);
        }
        if (r.takeMoney) {
            logResult("loot money");
        }
        if (r.closed) {
            logResult("loot closed");
        }
        break;
    }
    case Page::Chat: {
        ui::Gui g{c.ui, c.draw};
        m_chatViews.clear();
        for (const std::string& chatLine : m_chatLines) {
            m_chatViews.push_back({chatLine, ui::kWhite});
        }
        ChatView view;
        view.lines = m_chatViews;
        view.input = m_chatInput;
        view.editing = m_chatEditing;
        view.channel = text::get(kChannels[m_chatChannel]);
        view.expanded = m_chatExpanded;
        const Rect& safe = c.safeArea;
        const f32 w = std::min(safe.w - g.px(16.0f), g.px(460.0f));
        const f32 h = m_chatExpanded ? std::min(safe.h - g.px(16.0f), g.px(300.0f)) : g.px(120.0f);
        const ChatResult r = chatWindow(c, {safe.x + g.px(8.0f), safe.bottom() - h - g.px(8.0f), w, h}, view);
        if (r.tappedInput) {
            m_chatEditing = !m_chatEditing;
            logResult(m_chatEditing ? "chat input focused" : "chat input unfocused");
        }
        if (r.cycleChannel) {
            m_chatChannel = (m_chatChannel + 1) % static_cast<u32>(std::size(kChannels));
            logResult("chat channel");
        }
        if (r.send && !m_chatInput.empty()) {
            m_chatLines.push_back("[Arthel] says: " + m_chatInput);
            m_chatInput = "One more line";
            logResult("chat send");
        }
        if (r.toggleExpanded) {
            m_chatExpanded = !m_chatExpanded;
            logResult(m_chatExpanded ? "chat expanded" : "chat collapsed");
        }
        break;
    }
    case Page::Menu: {
        const MenuChoice choice = gameMenu(c);
        if (choice != MenuChoice::None) {
            std::snprintf(line, sizeof(line), "menu choice %u", static_cast<u32>(choice));
            logResult(line);
        }
        break;
    }
    case Page::Settings: {
        const SettingsView before = m_settings;
        if (settingsPanel(c, m_settings)) {
            logResult("settings closed");
        }
        if (!(before == m_settings)) {
            std::snprintf(line, sizeof(line), "settings %.2f %.0f %d", m_settings.uiScale, m_settings.viewDistance,
                          m_settings.showFps);
            logResult(line);
        }
        break;
    }
    case Page::Confirm: {
        // The longest of the game's confirmations: the spirit healer's (CONFIRM_XP_LOSS).
        char question[320], duration[32];
        text::format(duration, "GENERIC_MIN_P1", 10);
        text::format(question, "CONFIRM_XP_LOSS", std::string_view(duration));
        const ConfirmResult r = confirmDialog(c, question, text::get("ACCEPT"), text::get("CANCEL"));
        if (r.yes) {
            logResult("confirm yes");
        }
        if (r.no) {
            logResult("confirm no");
        }
        break;
    }
    case Page::Widgets: widgetsPage(c); break;
    case Page::Login: m_glue.draw(c, *m_text); break; // phase 18 (glue): AccountLogin
    case Page::Combat: m_combat.draw(c); break;
    case Page::Create:
    case Page::Party:
    case Page::SocialChat:
    case Page::Emotes:
    case Page::Sheet:
    case Page::Social:
    case Page::Channels:
        socialPage(c, static_cast<u32>(m_page) - static_cast<u32>(Page::Create));
        break;
    case Page::Bags: m_inventory.draw(c, InventoryGallery::Page::Bags); break;
    case Page::Character: // phase 15 (character)
        m_character.setModels(&m_modelFrames);
        m_character.draw(c, CharacterGallery::Page::Character);
        break;
    case Page::ItemTooltip: m_inventory.draw(c, InventoryGallery::Page::Tooltip); break;
    case Page::Vendor: m_inventory.draw(c, InventoryGallery::Page::Vendor); break;
    case Page::Bank: m_inventory.draw(c, InventoryGallery::Page::Bank); break;
    case Page::Reading: m_inventory.draw(c, InventoryGallery::Page::Reading); break;
    case Page::Quantity: m_inventory.draw(c, InventoryGallery::Page::Quantity); break;
    case Page::Durability: m_inventory.draw(c, InventoryGallery::Page::Durability); break;
    case Page::Inspect: // phase 13 (social), phase 15 (character): the model and the honor tab
        m_character.setModels(&m_modelFrames);
        m_character.draw(c, CharacterGallery::Page::Inspect);
        break;
    case Page::WorldMap:
    case Page::Continent:
    case Page::Taxi:
    case Page::Loading:
    case Page::ZoneText: travelPage(c); break;
    case Page::Rolls: m_exchange.draw(c, ExchangeGallery::Page::Rolls, m_ui.nowMs()); break;
    case Page::Mail: m_exchange.draw(c, ExchangeGallery::Page::Mail, m_ui.nowMs()); break;
    case Page::MailSend: m_exchange.draw(c, ExchangeGallery::Page::MailSend, m_ui.nowMs()); break;
    case Page::Trade: m_exchange.draw(c, ExchangeGallery::Page::Trade, m_ui.nowMs()); break;
    case Page::Auction: m_exchange.draw(c, ExchangeGallery::Page::Auction, m_ui.nowMs()); break;
    case Page::AuctionBids: m_exchange.draw(c, ExchangeGallery::Page::AuctionBids, m_ui.nowMs()); break;
    case Page::AuctionSell: m_exchange.draw(c, ExchangeGallery::Page::AuctionSell, m_ui.nowMs()); break;
    case Page::MailEdit: m_exchange.draw(c, ExchangeGallery::Page::MailEdit, m_ui.nowMs()); break;
    case Page::Portraits: m_portraits.draw(c); break;
    case Page::Messages: m_messages.draw(c, false, m_ui.nowMs()); break;
    case Page::MessageLog: m_messages.draw(c, true, m_ui.nowMs()); break;
    case Page::Units: m_unitFrames.draw(c, UnitsGallery::Page::Frames, m_ui.nowMs()); break;
    case Page::UnitTips: m_unitFrames.draw(c, UnitsGallery::Page::Tooltips, m_ui.nowMs()); break;
    case Page::UnitMenu: m_unitFrames.draw(c, UnitsGallery::Page::Menu, m_ui.nowMs()); break;
    case Page::UnitMenuSelf: m_unitFrames.draw(c, UnitsGallery::Page::MenuSelf, m_ui.nowMs()); break;
    case Page::UnitMenuCreature: m_unitFrames.draw(c, UnitsGallery::Page::MenuCreature, m_ui.nowMs()); break;
    case Page::WorldLabels: m_worldLabels.draw(c); break;
    case Page::Auras: m_auras.draw(c, m_ui.nowMs()); break;
    case Page::Popups: m_popups.draw(c, 0, m_ui.nowMs()); break;
    case Page::Popups2: m_popups.draw(c, 1, m_ui.nowMs()); break;
    case Page::Popups3: m_popups.draw(c, 2, m_ui.nowMs()); break;
    case Page::Popups4: m_popups.draw(c, 3, m_ui.nowMs()); break;
    case Page::ReadyCheck: m_raidAlerts.draw(c, RaidAlertGallery::Page::ReadyCheck, m_ui.nowMs()); break; // phase 16
    case Page::RaidWarning: m_raidAlerts.draw(c, RaidAlertGallery::Page::RaidWarning, m_ui.nowMs()); break;
    case Page::MeetingStone: m_raidAlerts.draw(c, RaidAlertGallery::Page::MeetingStone, m_ui.nowMs()); break;
    case Page::Targeting: m_targeting.draw(c, 0, m_ui.nowMs()); break; // phase 13 (spells)
    case Page::Targeting2: m_targeting.draw(c, 1, m_ui.nowMs()); break;
    case Page::RaidInfo: {
        // Sample saves (Map.dbc names, instance ids) with their "Resets in" as RaidInfoFrame_Update writes it.
        struct Sample {
            const char* name;
            const char* id;
            u32 seconds;
        };
        static constexpr Sample kSamples[] = {{"Molten Core", "1093", 6 * 86400 + 23 * 3600 + 5 * 60},
                                              {"Onyxia's Lair", "1101", 2 * 86400 + 5 * 3600},
                                              {"Zul'Gurub", "1187", 86400 + 4 * 3600 + 30 * 60}};
        static std::array<std::array<char, 96>, std::size(kSamples)> resets{};
        std::array<RaidInfoRowView, std::size(kSamples)> rows{};
        for (u32 i = 0; i < std::size(kSamples); ++i) {
            char time[64];
            const std::string_view when = game::secondsToTime(time, kSamples[i].seconds);
            const std::string_view label = text::get("RESETS_IN");
            std::snprintf(resets[i].data(), resets[i].size(), "%.*s %.*s", static_cast<int>(label.size()),
                          label.data(), static_cast<int>(when.size()), when.data());
            rows[i] = {kSamples[i].name, kSamples[i].id, resets[i].data()};
        }
        raidInfoPanel(c, rows);
        break;
    }
    case Page::Raid: m_raid.draw(c, m_ui.nowMs()); break; // phase 16 (raidframe)
    case Page::TradeSkill: m_tradeSkills.draw(c, TradeSkillGallery::Page::TradeSkill); break;
    case Page::Craft: m_tradeSkills.draw(c, TradeSkillGallery::Page::Craft); break;
    case Page::BeastTraining: m_tradeSkills.draw(c, TradeSkillGallery::Page::BeastTraining); break;
    case Page::QuestWatch: m_quests.drawWatch(c, static_cast<u32>(m_ui.nowMs())); break; // phase 13
    case Page::ModelFrame: m_modelFrames.draw(c); break; // phase 15
    case Page::World: travelPage(c); break;              // phase 15: the World map
    case Page::PetBook:
    case Page::PetBar:
    case Page::PartyPets:
    case Page::Stable:
    case Page::PetPaperDoll:
    case Page::Feedback: // phase 15 (pets)
        m_pets.setModels(&m_modelFrames);
        m_pets.draw(c, static_cast<PetGallery::Page>(static_cast<u32>(m_page) - static_cast<u32>(Page::PetBook)),
                    m_ui.nowMs());
        break;
    case Page::Containers: m_items.draw(c, ItemsGallery::Page::Containers); break; // phase 15 (items)
    case Page::Compare: m_items.draw(c, ItemsGallery::Page::Compare); break;
    case Page::DressUp: m_items.draw(c, ItemsGallery::Page::DressUp); break;
    case Page::AuctionDressUp: {
        // The auction house with its dressing room docked at its right edge, as the game's panels place them.
        const ui::Rect place = c.panel.w > 0.0f ? c.panel : c.safeArea.inset(m_ui.px(6.0f));
        const f32 dock = DressUpScreens::auctionDockWidth(c, place.w);
        DialogContext auction = c;
        auction.panel = {place.x, place.y, place.w - dock, place.h};
        auction.panelAlign = PanelAlign::TopLeft;
        auction.profile = uiPanelWide(auction.panel) ? ScreenProfile::Wide : ScreenProfile::Square;
        m_exchange.draw(auction, ExchangeGallery::Page::Auction, m_ui.nowMs());
        m_items.drawAuctionDock(c, {place.right() - dock, place.y, dock, place.h});
        break;
    }
    case Page::MasterLoot: m_items.draw(c, ItemsGallery::Page::MasterLoot); break;
    case Page::MasterLootRaid: m_items.draw(c, ItemsGallery::Page::MasterLootRaid); break;
    case Page::LootPopups: m_items.draw(c, ItemsGallery::Page::LootPopups); break;
    case Page::BankBags: m_items.draw(c, ItemsGallery::Page::BankBags); break;
    case Page::Reputation: // phase 15 (character)
    case Page::Skills:
    case Page::Honor:
    case Page::InspectHonor: {
        static constexpr CharacterGallery::Page kPages[4] = {
            CharacterGallery::Page::Reputation, CharacterGallery::Page::Skills, CharacterGallery::Page::Honor,
            CharacterGallery::Page::InspectHonor};
        m_character.setModels(&m_modelFrames);
        m_character.draw(c, kPages[static_cast<u32>(m_page) - static_cast<u32>(Page::Reputation)]);
        break;
    }
    case Page::Guild: m_guild.draw(c); break;             // phase 15 (guild, help)
    case Page::Macro: m_options.draw(c, *m_text, OptionsGallery::Page::Macro); break; // phase 15 (options)
    case Page::MacroPopup: m_options.draw(c, *m_text, OptionsGallery::Page::MacroPopup); break;
    case Page::UiOptions: m_options.draw(c, *m_text, OptionsGallery::Page::UiOptions); break;
    case Page::UiOptionsAdvanced: m_options.draw(c, *m_text, OptionsGallery::Page::UiOptionsAdvanced); break;
    case Page::SoundOptions: m_options.draw(c, *m_text, OptionsGallery::Page::SoundOptions); break;
    case Page::CombatText: m_options.draw(c, *m_text, OptionsGallery::Page::CombatText); break;
    case Page::Pullouts: m_raidPullouts.draw(c, RaidPulloutGallery::Page::Pullouts, m_ui.nowMs()); break; // phase 16
    case Page::Pullouts2: m_raidPullouts.draw(c, RaidPulloutGallery::Page::All, m_ui.nowMs()); break;
    case Page::Pvp: m_pvp.draw(c, m_ui.nowMs()); break; // phase 17 (honor)
    case Page::Battlefield: m_battlefield.draw(c, m_ui.nowMs()); break; // phase 17 (queue)
    case Page::WorldState: m_worldStates.draw(c, m_ui.nowMs()); break; // phase 17 (scores)
    // Phase 18 (glue).
    case Page::Glue: m_glue.draw(c, *m_text); break;
    // Phase 18 (create).
    case Page::CharCreate: m_charCreate.draw(c, *m_text); break;
    // Phase 18 (p3).
    case Page::P3: m_p3.draw(c, m_ui.nowMs()); break;
    // Phase 18 (extras).
    case Page::Extras: m_extras.draw(c, m_ui.nowMs()); break;
    case Page::Windows: m_windows.draw(c, m_ui.nowMs()); break; // retouches (windows)
    case Page::UiModels: m_uiModels.draw(c, m_ui.nowMs()); break; // retouches (ui models)
    case Page::AppOnly: appOnlyGalleryPage(c); break; // phase 22 (apppack)
    case Page::Count: break;
    }
}

void UiGallery::setTravelData(const game::AreaData* areas, const game::WorldMapData* maps,
                              const game::TaxiData* taxi) {
    m_areas = areas;
    m_travel.init(areas, maps, taxi, m_resources);
    m_quests.setAreas(areas); // phase 13: the quest log's zone headers
}

void UiGallery::travelPage(DialogContext& c) {
    // Goldshire's inn, facing east (the map's arrow), with the corpse by the Stone Cairn Lake road.
    TravelScreens::MapSubject subject;
    subject.hasPlayer = true;
    subject.map = 0;
    subject.position = {-9459.0, 42.0, 57.0};
    subject.facing = -1.5708f;
    subject.hasCorpse = true;
    subject.corpseMap = 0;
    subject.corpseX = -9350.0f;
    subject.corpseY = -400.0f;
    subject.currentTaxiNode = 2;
    // Phase 15: two party members by Goldshire and one in Orgrimmar (the World map shows them all).
    static constexpr TravelScreens::MapSubject::Member kMembers[] = {
        {"Ugha", 0, -9400.0f, -40.0f}, {"Tinka", 0, -9530.0f, 120.0f}, {"Brakk", 1, 1600.0f, -4400.0f}};
    subject.members = kMembers;
    subject.playerName = "Arthel";
    switch (m_page) {
    case Page::WorldMap: m_travel.previewWorldMap(c, 30, subject); break;  // Elwynn Forest
    case Page::Continent: m_travel.previewWorldMap(c, 14, subject); break; // Eastern Kingdoms
    case Page::World: m_travel.previewWorldMap(c, game::WorldMapData::kWorldMap, subject); break; // phase 15
    case Page::Taxi: {
        // Stormwind's flight master, knowing most Alliance paths of the Eastern Kingdoms.
        game::TaxiMask known{};
        for (const u32 node : {2u, 4u, 5u, 6u, 7u, 8u, 12u, 14u, 16u, 19u, 43u, 45u, 66u, 67u, 71u, 74u}) {
            known[node / 32] |= 1u << (node % 32);
        }
        m_travel.previewTaxiMap(c, 2, known, 1500);
        break;
    }
    case Page::Loading: {
        const f32 elapsed = static_cast<f32>(c.ui.nowMs() - m_startMs) / 6000.0f;
        // Phase 20 (retouches): each time the bar starts again, the next of GameTips.dbc's tips.
        if (const u32 cycle = static_cast<u32>(elapsed); cycle != m_loadingCycle) {
            m_loadingCycle = cycle;
            m_travel.nextLoadingTip();
        }
        m_travel.drawLoading(c, 1, std::fmod(elapsed, 1.0f));
        break;
    }
    case Page::ZoneText: {
        ZoneTextView view;
        view.zone = "Elwynn Forest";
        view.subzone = "Goldshire";
        char pvp[64];
        view.pvp = game::zonePvpText(pvp, game::ZonePvp::Friendly, game::kAreaTeamAlliance);
        view.color = rgba(26, 255, 26);
        view.ageMs = static_cast<u32>((c.ui.nowMs() - m_startMs) % kZoneTextMs);
        zoneText(c, c.safeArea, view);
        break;
    }
    default: break;
    }
}

void UiGallery::widgetsPage(DialogContext& c) {
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    ui::modalBlocker(g, widgetId("widgets.blocker"), 0.25f);
    const f32 pad = g.px(12.0f);
    const bool wide = c.profile == ScreenProfile::Wide;
    // Left: a dialog panel with the controls; right: parchment with a long scrolling list; the tracker at the
    // edge.
    const f32 trackerW = g.px(200.0f);
    const Rect area{safe.x + pad, safe.y + g.px(48.0f), safe.w - 3.0f * pad - trackerW,
                    safe.h - g.px(48.0f) - pad};
    const Rect panel{area.x, area.y + g.unit(12.0f), wide ? area.w * 0.58f : area.w,
                     wide ? area.h - g.unit(12.0f) : area.h * 0.55f};
    ui::dialogPanel(g, panel);
    ui::dialogHeader(g, panel, "Controls");
    const Rect content = ui::dialogContent(g, panel);
    const ui::ScrollView view =
        ui::beginScroll(g, widgetId("widgets.controls"),
                        {content.x, content.y + g.px(16.0f), content.w, content.h - g.px(16.0f)});
    f32 y = view.top + g.px(4.0f);
    const f32 x = content.x + g.px(6.0f), w = content.w - g.px(12.0f);
    char line[64];
    // Buttons.
    const f32 bw = std::min(g.px(150.0f), (w - g.px(12.0f)) / 3.0f);
    if (ui::button(g, widgetId("widgets.button"), {x, y, bw, g.px(40.0f)}, text::get("ACCEPT"))) {
        logResult("widgets button");
    }
    ui::button(g, widgetId("widgets.disabled"), {x + bw + g.px(6.0f), y, bw, g.px(40.0f)}, "Disabled", false);
    if (ui::closeButton(g, widgetId("widgets.close"),
                        {x + 2.0f * (bw + g.px(6.0f)), y + g.px(4.0f), g.px(32.0f), g.px(32.0f)})) {
        logResult("widgets close");
    }
    if (ui::plusMinusButton(
            g, widgetId("widgets.plus"),
            {x + 2.0f * (bw + g.px(6.0f)) + g.px(52.0f), y + g.px(9.0f), g.px(22.0f), g.px(22.0f)},
            !m_expanded)) {
        m_expanded = !m_expanded;
        logResult("widgets plusminus");
    }
    y += g.px(52.0f);
    if (ui::checkbox(g, widgetId("widgets.check"), {x + g.px(4.0f), y, w * 0.6f, g.px(44.0f)},
                     text::get("SHOW_UNIT_NAMES"), m_check)) {
        logResult(m_check ? "widgets check on" : "widgets check off");
    }
    y += g.px(50.0f);
    std::snprintf(line, sizeof(line), "Volume: %.0f%%", m_slider * 100.0f);
    ui::label(g, {x, y, w, g.px(20.0f)}, line, ui::gameFont(g.px(15.0f)));
    y += g.px(20.0f);
    if (ui::slider(g, widgetId("widgets.slider"), {x + g.px(4.0f), y, w - g.px(8.0f), g.px(40.0f)}, m_slider,
                   0.0f, 1.0f, 0.05f)) {
        std::snprintf(line, sizeof(line), "widgets slider %.2f", m_slider);
        logResult(line);
    }
    y += g.px(48.0f);
    const bool blink = ((c.ui.nowMs() - m_startMs) / 3000) % 2 == 0;
    if (ui::textField(g, widgetId("widgets.field"), {x, y, w, g.px(40.0f)}, blink ? "Arthel" : "",
                      "Character name", blink)) {
        logResult("widgets field");
    }
    y += g.px(52.0f);
    // Item slots: loading, count, quality, cooldown sweep, unusable, empty, selected.
    const f32 slot = g.px(40.0f), gap = g.px(12.0f);
    const f32 cooldown = 1.0f - std::fmod(static_cast<f32>(c.ui.nowMs() - m_startMs) / 6000.0f, 1.0f);
    const u64 icons[] = {icon("INV_Misc_Food_15"), icon("INV_Sword_04"),  icon("INV_Bracer_02"),
                         icon("Spell_Holy_Heal"),  icon("INV_Hammer_09"), 0,
                         icon("INV_Misc_Bag_07")};
    for (u32 i = 0; i < std::size(icons); ++i) {
        ui::SlotView s;
        s.icon = icons[i] ? c.resources.icon(icons[i]) : std::nullopt;
        s.count = i == 0 ? 20 : i == 6 ? 3 : 1;
        s.quality = i == 2 ? 2 : i == 4 ? 4 : 1;
        s.cooldown = i == 3 ? cooldown : 0.0f;
        s.usable = i != 4;
        s.empty = i == 5;
        s.selected = i == 6;
        const ui::WidgetState state =
            ui::itemSlot(g, widgetId("widgets.slot", i),
                         {x + gap * 0.5f + static_cast<f32>(i) * (slot + gap), y, slot, slot}, s);
        if (state.clicked) {
            std::snprintf(line, sizeof(line), "widgets slot %u", i);
            logResult(line);
        }
    }
    y += slot + g.px(12.0f);
    // Money, as the MoneyFrame shows it.
    const u32 amounts[] = {0, 7, 105, 120003, 1234567};
    f32 mx = x;
    for (const u32 amount : amounts) {
        mx += ui::money(g, mx, y + g.px(10.0f), amount, g.px(14.0f), ui::Align::Left) + g.px(18.0f);
    }
    y += g.px(28.0f);
    ui::tooltipPanel(g, {x, y, w, g.px(64.0f)});
    ui::label(g, {x + g.px(10.0f), y + g.px(6.0f), w - g.px(20.0f), g.px(24.0f)}, "Wolfskin Bracers",
              ui::gameFont(g.px(15.0f), ui::qualityColor(2)));
    ui::label(g, {x + g.px(10.0f), y + g.px(30.0f), w - g.px(20.0f), g.px(24.0f)}, "Wrist    Leather",
              ui::gameFont(g.px(14.0f), ui::kWhite));
    y += g.px(72.0f);
    ui::endScroll(g, view, y - view.top);

    // A long list on parchment (drag and fling it).
    const Rect listRect =
        wide ? Rect{panel.right() + pad, area.y, area.right() - panel.right() - pad, area.h}
             : Rect{area.x, panel.bottom() + pad, area.w, area.bottom() - panel.bottom() - pad};
    ui::parchment(g, listRect);
    const ui::ScrollView list = ui::beginScroll(g, widgetId("widgets.list"), listRect.inset(g.px(4.0f)));
    f32 ly = list.top + g.px(4.0f);
    for (u32 i = 0; i < 40; ++i) {
        ui::RowStyle style;
        style.text = ui::questFont(g.px(15.0f));
        style.selected = static_cast<i32>(i) == m_rowSelected;
        style.icon = i % 3 == 0   ? assetPathHash("Interface\\GossipFrame\\AvailableQuestIcon.blp")
                     : i % 3 == 1 ? assetPathHash("Interface\\GossipFrame\\ActiveQuestIcon.blp")
                                  : assetPathHash("Interface\\GossipFrame\\GossipGossipIcon.blp");
        std::snprintf(line, sizeof(line), "Row %u of the test list", i + 1);
        if (ui::listRow(g, widgetId("widgets.row", i), {list.viewport.x, ly, list.viewport.w, g.px(44.0f)},
                        line, style)) {
            m_rowSelected = static_cast<i32>(i);
            std::snprintf(line, sizeof(line), "widgets row %u", i);
            logResult(line);
        }
        ly += g.px(44.0f);
    }
    ui::endScroll(g, list, ly - list.top + g.px(4.0f));

    // The watched quests (QuestWatchFrame, phase 13), right edge.
    std::array<QuestWatchEntryView, 3> watched{};
    for (u32 i = 0; i < watched.size(); ++i) {
        watched[i] = {kQuestLog[i].title, kQuestLog[i].objectives};
    }
    const f32 watchW = std::min(safe.w * 0.3f, g.px(260.0f));
    const i32 tracked =
        questWatchFrame(c, safe.right() - pad - watchW, area.y, area.bottom(), watchW, watched).tapped;
    if (tracked >= 0) {
        std::snprintf(line, sizeof(line), "tracker %d", tracked);
        logResult(line);
    }
}

} // namespace vd
