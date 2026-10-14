#pragma once

#include "app/AuraGallery.h"
#include "app/BattlefieldGallery.h"
#include "app/CharacterGallery.h"
#include "app/CombatGallery.h"
#include "app/ExchangeGallery.h"
#include "app/GuildGallery.h"
#include "app/Dialogs.h"
#include "app/InventoryGallery.h"
#include "app/ItemsGallery.h"
#include "app/PortraitGallery.h"
#include "app/MessagesGallery.h"
#include "app/ModelFrameGallery.h"
#include "app/PetGallery.h"
#include "app/OptionsGallery.h"
#include "app/RaidPulloutGallery.h"
#include "app/PopupGallery.h"
#include "app/PvpGallery.h" // phase 17 (honor)
// phase 18 (glue) includes
#include "app/GlueGallery.h"
// phase 18 (create) includes
#include "app/CharCreateGallery.h"
// phase 18 (p3) includes
#include "app/P3Gallery.h"
// phase 18 (extras) includes
#include "app/ExtrasGallery.h"
#include "app/UiModelsGallery.h" // retouches (ui models)
#include "app/TradeSkillGallery.h"
#include "app/TargetingGallery.h"
#include "app/QuestGallery.h"
#include "app/RaidGallery.h"
#include "app/RaidAlertGallery.h"
#include "app/WorldStateGallery.h"
#include "app/WindowsGallery.h" // retouches (windows)
#include "app/WorldLabelsGallery.h"
#include "app/TravelScreens.h"
#include "app/UnitsGallery.h"
#include "core/Types.h"
#include "platform/Input.h"
#include "render/Renderer.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <span>
#include <string>
#include <vector>

namespace vd {

class UiResources;

// Development (phase 7): the touch UI's dialogs (app/Dialogs) with Northshire sample data, drawn every frame
// instead of the HUD when the environment variable VD_UI_GALLERY names a page: gossip, details, progress,
// reward, log, loot, chat, menu, settings, confirm, widgets, login, or all (a NEXT button cycles through
// them). Sample data is English, as the server sends it; the interface's words come from core/Text. What the
// player does goes to the log ("UI gallery: ..."). The screen's shape picks the profile (Wide from 1.6:1).
class UiGallery {
public:
    // Reads VD_UI_GALLERY; stays inactive when it is not set (or names no page).
    bool init(UiResources* resources, u32 maxQuads);
#if defined(VD_DIST)
    bool active() const { return false; } // phase 22: no gallery in the published build (its pages compile away)
#else
    bool active() const { return m_resources != nullptr; }
#endif
    // Phase 8: the travel pages' tables (worldmap, continent, taxi, loading, zonetext).
    void setTravelData(const game::AreaData* areas, const game::WorldMapData* maps, const game::TaxiData* taxi);
    // Parity: the portraits page's sample units and their images (app/PortraitGallery).
    void setPortraits(Actors* actors, Portraits* portraits) { m_portraits.setSources(actors, portraits); }
    // Phase 17 (scores): the game's world state tables and the battlegrounds' map art for its pages.
    void setWorldStates(const WorldStateScreens* game) { m_worldStates.setSource(game, m_resources); }
    // Phase 18 (extras): the game's languages (LanguageWords...) for the "languages" page.
    void setLanguageData(const game::LanguageData* data) { m_extras.setLanguageData(data); }
    // Phase 15: the model frames' images and the sample models other pages show (app/ModelFrameGallery).
    void setModelViews(Actors* actors, ModelViews* views) {
        m_modelFrames.setSources(actors, views);
        m_items.setModelFrames(&m_modelFrames); // phase 15 (items): the dressing rooms' sample model
        m_guild.setSources(actors, views); // phase 15 (guild): the tabard designer's model
    }

    void touchEvent(TouchPhase phase, i32 id, f32 x, f32 y) { m_ui.touchEvent(phase, id, x, y); }
    // `text`: the platform's keyboard (the login page edits through it).
    void update(f32 width, f32 height, f32 dp, const ui::Rect& safeArea, u64 nowMs, ui::TextInput& text);

    std::span<const UiVertex> vertices() const { return m_draw.vertices(); }
    std::span<const UiDrawCmd> commands() const { return m_draw.commands(); }

private:
    enum class Page : u8 {
        Gossip,
        Details,
        Progress,
        Reward,
        Log,
        Loot,
        Chat,
        Menu,
        Settings,
        Confirm,
        Widgets,
        Login,
        Combat, // phase 8: CombatGallery's pages (spellbook, trainer, talents, pet...)
        // Phase 8, social (app/SocialGallery.cpp).
        Create,
        Party,
        SocialChat,
        Emotes,
        Sheet,
        Social,
        Channels,
        // Phase 8: items (app/InventoryGallery).
        Bags,
        Character,
        ItemTooltip,
        Vendor,
        Bank,
        Reading,
        Quantity,
        Durability,
        Inspect, // phase 13 (social): InspectFrame
        // Phase 8: travel.
        WorldMap,
        Continent,
        Taxi,
        Loading,
        ZoneText,
        // Phase 8: group loot, mail, trade, the auction house (app/ExchangeGallery).
        Rolls,
        Mail,
        MailSend,
        Trade,
        Auction,
        AuctionBids,
        AuctionSell,
        MailEdit,
        Portraits, // parity: the unit frames with real portraits (app/PortraitGallery)
        // Parity: the client's own lines where 1.12 shows them, the mirror timers (app/MessagesGallery).
        Messages,
        MessageLog,
        // Parity: unit frames, unit tooltips, the player's menu, the experience bar (app/UnitsGallery).
        Units,
        UnitTips,
        UnitMenu,
        UnitMenuSelf,     // phase 13 (social): unitmenu2
        UnitMenuCreature, // phase 13 (social): unitmenu3
        WorldLabels, // parity: names, plates and chat bubbles over units (app/WorldLabelsGallery)
        Auras, // parity: buffs, debuffs, weapon enchantments and their tooltips (app/AuraGallery)
        // Parity: the StaticPopup stack (app/PopupGallery).
        Popups,
        Popups2,
        Popups3,
        Popups4,  // phase 12: the instances' popups
        RaidInfo, // phase 12: RaidInfoFrame
        Raid,     // phase 16 (raidframe): the raid tab (app/RaidGallery's pages)
        // Phase 16 (alerts): app/RaidAlertGallery.
        ReadyCheck,
        RaidWarning,
        MeetingStone,
        // Phase 13 (professions, app/TradeSkillGallery).
        TradeSkill,
        Craft,
        BeastTraining,
        // Phase 13 (spells): a spell waiting for its place or item (app/TargetingGallery).
        Targeting,
        Targeting2,
        QuestWatch, // phase 13 (quests): QuestWatchFrame and QuestTimerFrame (app/QuestGallery)
        ModelFrame, // phase 15: the interface's 3D model frames (app/ModelFrameGallery)
        World,      // phase 15: the World map (both continents) with the party
        // Phase 15 (pets): app/PetGallery.
        PetBook,
        PetBar,
        PartyPets,
        Stable,
        PetPaperDoll,
        Feedback,
        // Phase 15 (items): app/ItemsGallery.
        Containers,
        Compare,
        DressUp,
        AuctionDressUp,
        MasterLoot,
        MasterLootRaid,
        LootPopups,
        BankBags,
        // Phase 15 (character): CharacterFrame's other tabs and the InspectFrame's honor (app/CharacterGallery).
        Reputation,
        Skills,
        Honor,
        InspectHonor,
        Guild,      // phase 15 (guild, help): app/GuildGallery's pages (guild, tabard, help, gmsurvey...)
        // Phase 15 (options): the macro window and its popup, UIOptionsFrame's two tabs, SoundOptionsFrame
        // (app/OptionsGallery).
        Macro,
        MacroPopup,
        UiOptions,
        UiOptionsAdvanced,
        SoundOptions,
        CombatText, // the floating combat text with sample messages (app/CombatTextScreens)
        // Phase 16 (pullouts): the raid's pullout frames and the party frames in a raid (app/RaidPulloutGallery).
        Pullouts,
        Pullouts2,
        Pvp, // phase 17 (honor): app/PvpGallery's pages (spirithealer, skinned, pvp)
        Battlefield, // phase 17 (queue): app/BattlefieldGallery's pages (battlefield.., bgbutton)
        WorldState, // phase 17 (scores): app/WorldStateGallery's pages (worldstate, score, score2, victory, bgminimap)
        // Phase 18 (glue).
        Glue, // app/GlueGallery's pages (login is Login's)
        // Phase 18 (create).
        CharCreate, // app/CharCreateGallery's pages (create2, create3, create4; "create" too)
        // Phase 18 (p3).
        P3, // app/P3Gallery's pages (tutorial, tutorial42, chatwindows, chatconfig, chatfilters, ...)
        // Phase 18 (extras).
        Extras, // app/ExtrasGallery's pages (languages, spellbookflash)
        // retouches (windows)
        Windows, // app/WindowsGallery's pages (talents2, mailinvoice, mailinvoice2, codpopup, slots)
        UiModels, // retouches (ui models): app/UiModelsGallery's pages (cooldowns, autocast, minimapping, ...)
        AppOnly,  // phase 22 (apppack): app.vdpk alone (app/AppOnlyGallery)
        Count
    };

    void drawPage(DialogContext& c);
    void widgetsPage(DialogContext& c);
    void socialPage(DialogContext& c, u32 page); // 0 create .. 6 channels (app/SocialGallery.cpp)
    void travelPage(DialogContext& c);

    UiResources* m_resources = nullptr;
    ui::Context m_ui;
    ui::DrawList m_draw;
    Page m_page = Page::Gossip;
    bool m_cycle = false; // "all": the button to the next page
    // Parity pass (panels): VD_UI_GALLERY_PANEL=left|right|both draws the page as a panel in that place of the
    // screen (app/UiPanels), with the action cluster and the stick's pad shaded (0: as a modal dialog).
    u8 m_panelPlace = 0;
    // State the dialogs keep through the caller.
    i32 m_rewardChoice = -1;
    QuestGallery m_quests; // phase 13: the quest log, watches and timers of an offline session
    RaidGallery m_raid;    // phase 16 (raidframe)
    BattlefieldGallery m_battlefield; // phase 17 (queue)
    SettingsView m_settings;
    std::vector<std::string> m_chatLines; // sample lines plus what is "sent"
    std::vector<ChatLineView> m_chatViews;
    std::string m_chatInput = "Hi, anyone up for killing Hogger?";
    bool m_chatEditing = false;
    bool m_chatExpanded = true;
    u32 m_chatChannel = 0;
    // Widgets page.
    bool m_check = true;
    f32 m_slider = 0.4f;
    i32 m_rowSelected = 1;
    bool m_expanded = true;
    u64 m_startMs = 0;
    u32 m_loadingCycle = 0; // phase 20 (retouches): the loading page's bar runs, a tip each
    ui::TextInput* m_text = nullptr; // the platform's keyboard (the glue pages edit through it)
    CombatGallery m_combat;
    InventoryGallery m_inventory; // phase 8: the item windows' pages
    ExchangeGallery m_exchange;   // phase 8: group loot, mail, trade, auction house
    PortraitGallery m_portraits;  // parity: unit frame portraits
    ModelFrameGallery m_modelFrames; // phase 15: model frames, and sample models for the pages that show one
    PetGallery m_pets;               // phase 15 (pets)
    ItemsGallery m_items;            // phase 15 (items): bags, comparisons, dressing rooms, master loot
    CharacterGallery m_character;    // phase 15 (character): the character window and the InspectFrame
    OptionsGallery m_options;        // phase 15 (options): macros and the option windows
    RaidPulloutGallery m_raidPullouts; // phase 16 (pullouts)
    MessagesGallery m_messages;   // parity: chat routing, UIErrorsFrame, mirror timers
    UnitsGallery m_unitFrames;    // parity: unit frames, unit tooltip, experience bar
    WorldLabelsGallery m_worldLabels; // parity: world feedback
    AuraGallery m_auras;          // parity: auras
    PopupGallery m_popups;        // parity: popups
    RaidAlertGallery m_raidAlerts; // phase 16 (alerts): ready check, raid warnings, meeting stones
    WorldStateGallery m_worldStates; // phase 17 (scores)
    TradeSkillGallery m_tradeSkills; // phase 13: the profession windows
    TargetingGallery m_targeting; // phase 13 (spells)
    GuildGallery m_guild;         // phase 15 (guild, help)
    PvpGallery m_pvp;             // phase 17 (honor)
    // Phase 18 (glue).
    GlueGallery m_glue; // AccountLogin, GlueDialog, RealmList, CharacterSelect and its dialogs
    // Phase 18 (create).
    CharCreateGallery m_charCreate;
    // Phase 18 (p3).
    P3Gallery m_p3;
    // Phase 18 (extras).
    ExtrasGallery m_extras;
    WindowsGallery m_windows; // retouches (windows)
    UiModelsGallery m_uiModels; // retouches (ui models)
    // Travel pages.
    TravelScreens m_travel;
    const game::AreaData* m_areas = nullptr;
};

} // namespace vd
