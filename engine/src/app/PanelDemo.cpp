#include "app/PanelDemo.h"

#include "app/HudShared.h"
#include "app/SocialWindows.h"
#include "app/TravelScreens.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <array>

namespace vd {
namespace {

constexpr u64 icon(const char* name) {
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

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// VD_HUD_DEMO=gossip: a quest giver's conversation (the gallery's Deputy Willem).
constexpr GossipOptionView kDemoGossipOptions[] = {{"I want to browse your goods.", 1}, {"Train me.", 3}};
constexpr GossipQuestView kDemoGossipQuests[] = {
    {"Brotherhood of Thieves", 4, false, false},
    {"A Threat Within", 1, true, true},
};

} // namespace

PanelDemo::PanelDemo() {
    m_spellbook.select("spellbook");
    m_talents.select("talents");
}

MicroMenuView PanelDemo::microView(bool bagsOpen, const UiPanelSlots& panels) const {
    MicroMenuView v;
    v.level = 12;
    v.talentPoints = 3;
    v.portraitArt = hud::racePortrait(1, 1);
    v.pushed[static_cast<u32>(MicroButton::Character)] = panels.shown(UiPanel::Character);
    v.pushed[static_cast<u32>(MicroButton::Spellbook)] = panels.shown(UiPanel::Spellbook);
    v.pushed[static_cast<u32>(MicroButton::Talents)] = panels.shown(UiPanel::Talents);
    v.pushed[static_cast<u32>(MicroButton::QuestLog)] = panels.shown(UiPanel::QuestLog);
    v.pushed[static_cast<u32>(MicroButton::Social)] = panels.shown(UiPanel::Friends);
    v.pushed[static_cast<u32>(MicroButton::WorldMap)] = panels.shown(UiPanel::WorldMap);
    v.pushed[static_cast<u32>(MicroButton::Help)] = panels.shown(UiPanel::Help); // phase 15 (help)
    v.pushed[static_cast<u32>(MicroButton::MainMenu)] =
        panels.shown(UiPanel::GameMenu) || panels.shown(UiPanel::Options);
    v.bags = {assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp"), icon("INV_Misc_Bag_09"),
              icon("INV_Misc_Bag_10"), 0, 0};
    v.keyring = true;
    v.freeSlots = 9;
    v.bagsOpen = bagsOpen;
    m_items.bagBarState(v); // phase 15 (items)
    return v;
}

void PanelDemo::draw(UiPanel panel, DialogContext& c, TravelScreens& travel) {
    switch (panel) {
    case UiPanel::None: m_items.drawContainers(c); break; // phase 15 (items): a ContainerFrame per bag
    case UiPanel::Character: m_character.draw(c, CharacterGallery::Page::Character); break; // phase 15
    case UiPanel::Spellbook: m_spellbook.draw(c); break;
    case UiPanel::Talents: m_talents.draw(c); break;
    case UiPanel::Help: m_help.drawPage(c, GuildGallery::Page::Help); break; // phase 15 (help)
    case UiPanel::QuestLog: {
        // Phase 13 (quests): the game's quest log over an offline session (app/QuestGallery).
        if (m_quests.drawLog(c, static_cast<u32>(c.ui.nowMs()))) {
            logResult("panel quest log closed");
        }
        break;
    }
    case UiPanel::Friends: {
        // Retouches (friends frame): FriendsListFrame with the 1.12's art and sample friends.
        char lines[3][96], infos[2][48];
        const FriendRowView rows[] = {
            {text::format(lines[0], "FRIENDS_LIST_TEMPLATE", "Aeryn", "Goldshire", ""),
             text::format(infos[0], "FRIENDS_LEVEL_TEMPLATE", 13, "Priest")},
            {text::format(lines[1], "FRIENDS_LIST_TEMPLATE", "Galen", "Westfall", text::get("CHAT_FLAG_AFK")),
             text::format(infos[1], "FRIENDS_LEVEL_TEMPLATE", 11, "Rogue")},
            {text::format(lines[2], "FRIENDS_LIST_OFFLINE_TEMPLATE", "Brannoc"), text::get("UNKNOWN")},
        };
        FriendsFrameView view;
        view.rows = rows;
        view.selected = 0;
        view.connected = true;
        view.guildEnabled = false;
        const FriendsFrameResult r = friendsFrame(c, view);
        if (r.closed) {
            logResult("panel social closed");
        }
        break;
    }
    case UiPanel::WorldMap: {
        // Goldshire's inn, facing east (as the gallery's world map page).
        TravelScreens::MapSubject subject;
        subject.hasPlayer = true;
        subject.position = {-9459.0, 42.0, 57.0};
        subject.facing = -1.5708f;
        travel.previewWorldMap(c, 30, subject);
        break;
    }
    case UiPanel::Merchant: m_inventory.draw(c, InventoryGallery::Page::Vendor); break; // VD_HUD_DEMO=merchant
    case UiPanel::Quest: { // VD_HUD_DEMO=gossip
        const GossipView view{"Deputy Willem",
                              "Hello there, warrior.  Normally I'd be out on the beat looking after the folk of "
                              "Stormwind, but a lot of the Stormwind guards are fighting in other lands.",
                              kDemoGossipOptions, kDemoGossipQuests};
        if (gossipDialog(c, view).closed) {
            logResult("panel gossip closed");
        }
        break;
    }
    case UiPanel::GameMenu: {
        const MenuChoice choice = gameMenu(c);
        if (choice != MenuChoice::None) {
            logResult("panel game menu choice");
        }
        break;
    }
    default: break;
    }
}

} // namespace vd
