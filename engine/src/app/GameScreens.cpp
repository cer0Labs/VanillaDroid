#include "app/GameScreens.h"

#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/PanelDemo.h"
#include "app/Portraits.h"
#include "app/UiResources.h"
#include "app/WorldTap.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/CombatText.h"
#include "game/GameObjectAnim.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;
using ui::rgba;


// UIErrorsFrame.lua: UI_ERROR_MESSAGE (1, 0.1, 0.1), UI_INFO_MESSAGE (1, 1, 0).
constexpr u32 kErrorColor = rgba(255, 26, 26);
constexpr u32 kInfoColor = rgba(255, 255, 0);
// Objects the context button offers: found this far, usable this near (the server's interaction distance plus a
// margin for the object's size).
constexpr f32 kObjectSearchYards = 12.0f;
constexpr f32 kObjectUseYards = 5.5f;
// Auto Loot (ours): the loot window shows what there is for a moment, then each thing goes to the bags with its own
// sound, one after another.
constexpr u32 kAutoLootFirstMs = 250;
constexpr u32 kAutoLootStepMs = 120;
constexpr u32 kAutoLootDone = ~0u;

f32 floatValue(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Parity: the client's cursor over what the context button acts on (Interface\Cursor): a sword to attack, a
// bag to loot, an NPC's service (repair, buy, train, fly; speak otherwise), an object's lock (herbs, veins, a
// lock to pick), the mailbox; a gear for the other objects.
u64 contextCursor(GameSession& session, const game::GameData* data, HudContextAction action, u64 guid) {
    const Entity* e = session.entities().find(guid);
    switch (action) {
    case HudContextAction::Attack:
    case HudContextAction::StopAttack: return assetPathHash("Interface\\Cursor\\Attack.blp");
    case HudContextAction::Loot: return assetPathHash("Interface\\Cursor\\Pickup.blp");
    case HudContextAction::Talk: {
        const u32 flags = e ? game::npcFlags(*e) : 0;
        return assetPathHash((flags & game::kNpcFlagRepair)         ? "Interface\\Cursor\\Repair.blp"
                             : (flags & game::kNpcFlagVendor)       ? "Interface\\Cursor\\Buy.blp"
                             : (flags & game::kNpcFlagTrainer)      ? "Interface\\Cursor\\Trainer.blp"
                             : (flags & game::kNpcFlagFlightMaster) ? "Interface\\Cursor\\Taxi.blp"
                                                                    : "Interface\\Cursor\\Speak.blp");
    }
    case HudContextAction::Use: {
        const u32 type = e ? game::gameObjectType(*e) : 0;
        if (type == game::kGameObjectMailbox) {
            return assetPathHash("Interface\\Cursor\\Mail.blp");
        }
        const game::GameObjectInfo* info = e ? session.gameObject(e->value(uf::OBJECT_FIELD_ENTRY)) : nullptr;
        constexpr u32 kLockPick = 1, kLockHerbalism = 2, kLockMining = 3; // LockType (SharedDefines.h)
        switch (info && data && type == game::kGameObjectChest ? data->lockOpenType(info->data[0]) : 0) {
        case kLockPick: return assetPathHash("Interface\\Cursor\\PickLock.blp");
        case kLockHerbalism: return assetPathHash("Interface\\Cursor\\GatherHerbs.blp");
        case kLockMining: return assetPathHash("Interface\\Cursor\\Mine.blp");
        default:
            return assetPathHash(type == game::kGameObjectChest ? "Interface\\Cursor\\Pickup.blp"
                                                                : "Interface\\Cursor\\Interact.blp");
        }
    }
    default: return 0;
    }
}

// The name of the first item of `items` the player carries fewer than `counts` of (reagents, totems); "" if none.
std::string_view missingItem(GameSession& session, std::span<const u32> items, std::span<const u32> counts) {
    const Entity* me = session.entities().find(session.playerGuid());
    for (usize i = 0; i < items.size() && me; ++i) {
        const u32 need = i < counts.size() ? std::max(1u, counts[i]) : 1u;
        if (items[i] != 0 && game::inventoryCount(session.entities(), *me, items[i]) < need) {
            const net::ItemQueryInfo* info = session.item(items[i]);
            return info ? std::string_view(info->name) : std::string_view("...");
        }
    }
    return {};
}

// The client's text of a spell failure (SpellCastResult, as net::spellCastResultName names it): SPELL_FAILED_<name>,
// ERR_OUT_OF_<power> when short of power, with what it names (a reagent, a spell focus, a form, an area) taken from
// the server's argument or the spell. Generic texts when that is unknown.
std::string_view castFailedText(std::span<char> out, GameSession& session, const game::GameData* data, u32 spellId,
                                std::string_view name, i32 argument) {
    const std::optional<game::SpellInfo> spell = data ? data->spell(spellId) : std::nullopt;
    if (name == "NO_POWER") {
        static constexpr text::Key kPower[] = {"ERR_OUT_OF_MANA", "ERR_OUT_OF_RAGE", "ERR_OUT_OF_FOCUS",
                                               "ERR_OUT_OF_ENERGY"};
        const u32 power = spell ? spell->powerType : 0;
        return text::format(out, power < 4 ? kPower[power] : text::Key("ERR_OUT_OF_HEALTH")); // -2: POWER_HEALTH
    }
    if (name == "ALREADY_AT_FULL_HEALTH") {
        return text::format(out, "ERR_SPELL_FAILED_ALREADY_AT_FULL_HEALTH");
    }
    if (name == "ALREADY_AT_FULL_POWER") { // "You are already at full Mana.": the player's own power
        static constexpr text::Key kPowerName[] = {"MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS"};
        const Entity* me = session.entities().find(session.playerGuid());
        const u32 power = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 3) : 0;
        return text::format(out, "ERR_SPELL_FAILED_ALREADY_AT_FULL_POWER_S",
                            text::get(power < 5 ? kPowerName[power] : text::Key("MANA")));
    }
    char keyName[64];
    std::snprintf(keyName, sizeof(keyName), "SPELL_FAILED_%.*s", static_cast<int>(name.size()), name.data());
    const text::Key key{std::string_view(keyName)};
    if (!text::has(key)) {
        return text::format(out, "SPELL_FAILED_UNKNOWN");
    }
    const std::optional<game::SpellRequirements> needs = data ? data->requirements(spellId) : std::nullopt;
    std::string_view what;
    if (name == "REAGENTS" || name == "TOTEMS") {
        what = !needs ? std::string_view{}
               : name == "REAGENTS" ? missingItem(session, needs->reagents, needs->reagentCounts)
                                    : missingItem(session, needs->totems, {});
        if (what.empty() && name == "REAGENTS") {
            return text::format(out, "ERR_SPELL_FAILED_REAGENTS_GENERIC");
        }
    } else if (name == "REQUIRES_SPELL_FOCUS") {
        what = data ? data->spellFocusName(argument != 0 ? static_cast<u32>(argument) : needs ? needs->spellFocus : 0)
                    : std::string_view{};
    } else if (name == "REQUIRES_AREA") {
        what = data ? data->areaName(static_cast<u32>(argument)) : std::string_view{};
        if (what.empty()) {
            return text::format(out, "SPELL_FAILED_NOT_HERE");
        }
    } else if (name == "ONLY_SHAPESHIFT") {
        for (u32 form = 1; needs && data && form <= 32 && what.empty(); ++form) {
            what = (needs->stances & (1u << (form - 1))) != 0 ? data->shapeshiftFormName(form) : std::string_view{};
        }
    } else if (name.starts_with("EQUIPPED_ITEM_CLASS")) {
        return text::format(out, "SPELL_FAILED_EQUIPPED_ITEM"); // the item subclass' name is not known here
    }
    return text::format(out, key, what.empty() ? std::string_view("...") : what);
}

// Text of a quest refusal (QuestFailedReasons, vMaNGOS QuestDef.h) as the client words it.
std::string_view questFailedText(std::span<char> out, u32 reason, std::string_view title) {
    const std::string_view quest = title.empty() ? std::string_view("...") : title;
    switch (reason) {
    case 1: return text::format(out, "ERR_QUEST_FAILED_LOW_LEVEL");
    case 4: // inventory full: the quest's own text when it is known
        return title.empty() ? text::format(out, "ERR_INV_FULL") : text::format(out, "ERR_QUEST_FAILED_BAG_FULL_S", title);
    case 6: return text::format(out, "ERR_QUEST_FAILED_WRONG_RACE");
    case 12: return text::format(out, "ERR_QUEST_ONLY_ONE_TIMED");
    case 13: return text::format(out, "ERR_QUEST_ALREADY_ON");
    case 17: return text::format(out, "ERR_QUEST_FAILED_MAX_COUNT_S", quest);
    case 20: return text::format(out, "ERR_QUEST_FAILED_MISSING_ITEMS");
    case 22: return text::format(out, "ERR_QUEST_FAILED_NOT_ENOUGH_MONEY");
    default: return text::format(out, "ERR_QUEST_NEED_PREREQS"); // 0, 2 and the rest
    }
}

} // namespace

void GameScreens::init(const game::GameData* data, UiResources* resources, const game::AbilityData* abilities) {
    m_data = data;
    m_combat.init(data, abilities, resources,
                  [](void* self, u32 color, bool big, const char* line) {
                      static_cast<GameScreens*>(self)->messageText(color, big, line);
                  },
                  this);
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_resources = resources;
    m_travel.setSettings(&m_settings); // phase 20 (retouches): the loading screens' tips
    // Phase 15 (character): the character window's tabs after the paper doll, as CharacterFrame orders them
    // (CHARACTER, PET, REPUTATION, SKILLS, HONOR). Phase 15 (pets): PET (PetPaperDollFrame) while HasPetUI
    // (PetTab_Update).
    using Tab = InventoryScreens::CharacterTab;
    m_inventory.registerCharacterTab(
        Tab::Pet, "PET",
        [](void* self, GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
            static_cast<PetScreens*>(self)->petTab(session, c, frame.l.body);
            return false;
        },
        &m_pets, [](void*, GameSession& session) { return PetScreens::hasPetUI(session); });
    m_inventory.registerCharacterTab(
        Tab::Reputation, "REPUTATION",
        [](void* self, GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
            static_cast<CharacterScreens*>(self)->reputationTab(session, c, frame);
            return false;
        },
        &m_character);
    m_inventory.registerCharacterTab(
        Tab::Skills, "SKILLS",
        [](void* self, GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
            return static_cast<CharacterScreens*>(self)->skillsTab(session, c, frame);
        },
        &m_character);
    m_inventory.registerCharacterTab(
        Tab::Honor, "HONOR",
        [](void* self, GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
            static_cast<CharacterScreens*>(self)->honorTab(session, c, frame);
            return false;
        },
        &m_character);
    m_inventory.setCharacterSheet(&m_character);
    m_unitMenus.setCharacterSheet(&m_character);
    m_character.setPopups(&m_popups); // UNLEARN_SKILL
    m_character.setMessageSink(
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    if (const char* demo = VD_DEV_ENV("VD_HUD_DEMO")) {
        m_demo = std::make_unique<PanelDemo>(); // development: the panels with sample data, offline
        static constexpr struct {
            std::string_view name;
            UiPanel panel;
        } kDemoPanels[] = {{"merchant", UiPanel::Merchant}, {"gossip", UiPanel::Quest},
                           {"character", UiPanel::Character}, {"spellbook", UiPanel::Spellbook},
                           {"talents", UiPanel::Talents}, {"questlog", UiPanel::QuestLog},
                           {"social", UiPanel::Friends}, {"worldmap", UiPanel::WorldMap},
                           {"gamemenu", UiPanel::GameMenu}, {"video", UiPanel::Options},
                           {"sound", UiPanel::SoundOptions}, {"interface", UiPanel::UIOptions}};
        for (const auto& p : kDemoPanels) {
            m_demoOpen = p.name == std::string_view(demo) ? p.panel : m_demoOpen;
        }
    }
    m_notices.init(data, abilities);
    m_tradeSkills.init(data, abilities, resources); // phase 13 (professions)
    m_auras.init(data, abilities); // parity: auras
    m_social.setAuras(&m_auras);
    m_social.setCombatLog(&m_combat); // parity: the chat's Combat Log tab
    m_popups.init(data);              // parity (popups): the StaticPopups
    m_inventory.setPopups(&m_popups); // DELETE_ITEM, EQUIP_BIND, AUTOEQUIP_BIND, USE_BIND
    m_minimap.setPopups(&m_popups);   // phase 16 (alerts): CONFIRM_LEAVE_QUEUE
    m_battlefield.init(&m_popups, &m_worldStateScreens); // phase 17 (queue)
    m_objectTooltip.init(data);       // phase 16 (alerts): the zones of the meeting stones' tooltips
    m_dressUp.setData(data);             // phase 15 (items): the dressing rooms
    m_targeting.init(data);              // phase 13 (spells)
    m_inventory.setTargeting(&m_targeting);
    m_exchange.setTargeting(&m_targeting);
    m_exchange.setPopups(&m_popups); // phase 15 (items): CONFIRM_LOOT_ROLL, CONFIRM_LOOT_DISTRIBUTION
    m_combat.setPopups(&m_popups);    // USE_BIND (the action bar), ABANDON_PET
    m_pets.init(data, // phase 15 (pets): the stable's ERR_NOT_ENOUGH_MONEY
                [](void* self, u32 color, bool big, const char* line) {
                    static_cast<GameScreens*>(self)->messageText(color, big, line);
                },
                this);
    // Phase 15 (options): the macros (their window, their files, the action buttons that hold one).
    m_macros.init(data, resources);
    m_touch.init(data, abilities, resources); // phase 20: the touch cluster
    m_combatText.init(data, abilities);
    m_macros.setChat(&m_social);
    m_combat.setMacros(&m_macros);
    // Phase 13 (social): the players' menus (UnitPopup) and the C functions of the slash commands.
    m_unitMenus.init(data);
    m_unitMenus.setPopups(&m_popups);
    m_unitMenus.setSocial(&m_social);
    m_unitMenus.setInventory(&m_inventory);
    m_unitMenus.setMessageSink(
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    m_social.setUnitMenus(&m_unitMenus);
    m_social.setPopups(&m_popups);
    if (const char* demo = VD_DEV_ENV("VD_POPUP_DEMO")) {
        m_popupDemoSet = static_cast<u32>(std::max(0, std::atoi(demo)));
    }
    if (const char* tutorial = VD_DEV_ENV("VD_P3_TUTORIAL")) { // phase 18 (p3): <n> (TUTORIAL<n>) or "all"
        const bool all = std::string_view(tutorial) == "all";
        const u32 first = all ? 0u : static_cast<u32>(std::clamp(std::atoi(tutorial), 1, 50) - 1);
        m_tutorialDemo = true;
        m_tutorialScreens.demo(first, all ? TutorialScreens::kMaxAlerts : 4, 0);
        if (!all) {
            m_tutorialScreens.open(first, first == static_cast<u32>(game::Tutorial::Welcome));
        }
    }
    m_popups.setMessageSink(
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    m_options.setTutorials(&m_tutorialScreens); // phase 18 (p3): SHOW_TUTORIALS, Reset Tutorials
    m_accountFiles.init(&m_macros, &m_social);  // phase 18 (p3): the account data's files
    m_accountFiles.setTouch(&m_touch);           // phase 20 (edit): the touch layout's server copy (type 6)
    // Phase 20 (assists): the unit frames a helpful spell may be dragged onto (the raid pullouts', the party's, the
    // pet's and the target of target's), the message lines, the ground circle.
    TouchAssists::Links touchLinks;
    touchLinks.unitAt = [](void* context, f32 x, f32 y, ui::Rect& frame) -> u64 {
        GameScreens& self = *static_cast<GameScreens*>(context);
        u64 unit = self.m_raidPullouts.unitAt(x, y, frame);
        unit = unit != 0 ? unit : self.m_social.partyUnitAt(x, y, frame);
        return unit != 0 ? unit : self.m_combat.unitAt(x, y, frame);
    };
    touchLinks.message = [](void* context, u32 color, const char* line) {
        static_cast<GameScreens*>(context)->messageText(color, false, line);
    };
    touchLinks.context = this;
    touchLinks.targeting = &m_targeting;
    m_touch.assists().link(touchLinks);
    m_social.chatConfig().setPopups(&m_popups);  // phase 18 (p3): NAME_CHAT
    m_popups.setChatNamer(
        [](void* self, u32 chatFrame, std::string_view name) {
            static_cast<SocialScreens*>(self)->nameChatWindow(chatFrame, name);
        },
        &m_social);
    m_items.reserve(16);
    m_choiceItems.reserve(8);
    m_lootItems.reserve(48);
    m_gossipOptions.reserve(32);
    m_gossipQuests.reserve(32);
    m_quests.setMessageSink( // phase 13 (quests): sharing, watching and fishing lines
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    // Phase 15 (guild, help): the guild tab, the registrar, the charter, the tabard designer; the help windows.
    m_guild.init(data, resources);
    m_guild.setPopups(&m_popups);
    m_guild.setUnitMenus(&m_unitMenus);
    m_guild.setSocialScreens(&m_social);
    m_guild.setMessageSink(
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    m_social.setGuild(&m_guild);
    m_social.setRaidPullouts(&m_raidPullouts); // phase 16 (raids)
    m_raidPullouts.init(data, abilities, &m_auras, &m_inventory, // phase 16 (pullouts): auras, items dropped, errors
                        [](void* self, u32 color, bool big, const char* line) {
                            static_cast<GameScreens*>(self)->messageText(color, big, line);
                        },
                        this);
    m_help.setPopups(&m_popups);
    m_help.setSocialScreens(&m_social);
    m_help.setMessageSink(
        [](void* self, u32 color, bool big, const char* line) {
            static_cast<GameScreens*>(self)->messageText(color, big, line);
        },
        this);
    m_popups.setHelpOpener( // HELP_TICKET's "Edit My Ticket": HelpFrame on its ticket page
        [](void* self, GameSession& session) {
            GameScreens* screens = static_cast<GameScreens*>(self);
            if (!screens->m_panels.shown(UiPanel::Help)) {
                screens->togglePanel(session, UiPanel::Help);
            }
            screens->m_help.openTicketPage(session);
        },
        this);
    for (std::string& s : m_expanded) {
        s.reserve(2048);
    }
    m_castName.reserve(64);
    m_dialogQuestTitle.reserve(128);
}

bool GameScreens::inWorld(const GameSession& session) {
    return session.inWorld();
}

ScreenProfile GameScreens::profile(const ui::Rect& screen) const {
    return screen.w >= screen.h * 1.6f ? ScreenProfile::Wide : ScreenProfile::Square;
}

// Phase 18 (p3): where TutorialFrame's buttons stand: over the cast bar (its border's top edge reaches 28 units above
// the bar; ours: the buttons start right above the bar itself).
f32 GameScreens::hudBottom(Hud& hud, const DialogContext& c) {
    const ui::Rect& bar = hud.castBarRect();
    return bar.h > 0.0f ? bar.y - c.ui.px(4.0f) : c.safeArea.bottom() - c.ui.px(76.0f);
}

DialogContext GameScreens::context(Hud& hud, const ui::Rect& area) {
    const ui::Context& ui = hud.context();
    DialogContext c{hud.context(), hud.drawList(), *m_resources, profile({0.0f, 0.0f, ui.width(), ui.height()}),
                    area};
    c.chatLink = &m_chatLink;
    c.edges = m_edges;
    return c;
}

// --- Login ----------------------------------------------------------------------------------------------------

void GameScreens::drawLogin(GameSession& session, Hud& hud, ui::TextInput& input, const ui::Rect& area) {
    VD_PROFILE_ZONE("GameScreens::login");
    m_cursorsPlayer = 0; // parity (popups): back at the character list, every popup and window state goes
    m_popups.leaveWorld();
    m_macros.leaveWorld(); // phase 15 (options): the window's text saved
    m_touch.leaveWorld();  // phase 20: the touch layout saved
    m_accountFiles.update(session, session.lastUpdateMs()); // phase 18 (p3): the store; leaving: the uploads go
    m_raidAlerts.leaveWorld(); // phase 16 (alerts)
    m_worldStateScreens.leaveWorld(); // phase 17 (scores)
    DialogContext c = context(hud, area);
    // Phase 18 (lead): the glue scene behind the screens (app/GlueScene): AccountLogin's Dark Portal, the race's place
    // of the character chosen (CharacterSelect) or being made (CharacterCreate), standing in it.
    m_glue = GlueSceneRequest{};
    m_glue.background = GlueBackground::MainMenu;
    if (m_social.creating()) { // phase 8: the creation screen instead of the list
        // Phase 18 (create): CharacterCreate fills m_glue (its race's scene, the character being made) and plays its
        // sounds; the frame it ends in (Back, created) the list draws instead.
        m_social.drawCreation(session, c, input, m_glue, m_uiSound);
        if (m_social.creating()) {
            return;
        }
    }
    // Phase 18 (glue): AccountLogin, GlueDialog, RealmList, CharacterSelect and its dialogs (app/GlueScreens).
    m_glueScreens.setLoadingScreen(&m_travel);
    const GlueScreens::Result r = m_glueScreens.draw(session, c, input, m_glue);
    if (r.create) {
        m_social.startCreation();
    }
    if (r.offline) {
        m_offline = true;
    }
}

ui::Rect GameScreens::questWatchPlace(const DialogContext& c, const ui::Rect& area) const {
    const ui::Rect& chat = m_social.chatRect();
    if (chat.w <= 0.0f || m_social.chatExpanded()) {
        return {};
    }
    const ui::Gui g{c.ui, c.draw};
    f32 top = std::max(area.y + g.unit(100.0f), m_combat.petFrameBottom());
    const ui::Rect& party = m_social.partyRect();
    if (party.h > 0.0f && party.x < chat.right() && party.right() > chat.x) {
        top = std::max(top, party.bottom());
    }
    top += g.px(6.0f);
    const f32 bottom = chat.y - g.px(6.0f);
    return bottom > top ? ui::Rect{chat.x, top, chat.w, bottom - top} : ui::Rect{};
}

// --- HUD contents ---------------------------------------------------------------------------------------------

void GameScreens::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    VD_PROFILE_ZONE("GameScreens::prepareHud");
    hud.chatRect = m_social.chatRect(); // parity: taps on the chat stay the chat's
    hud.questWatchRect = m_questWatchRect;
    m_options.prepareHud(&session, m_settings, hud); // phase 15 (options): names, bars' text, extra bars...
    applyOptions(session);
    // Parity pass (panels): the bottom left action bar gives way to a window on the right (last frame's); the micro
    // menu's character button shows the player's portrait.
    hud.showExtraBar = hud.showExtraBar && !m_rightPlaceUsed;
    hud.unitFramesCovered = m_framesCovered; // ours: a window over them (last frame's)
    m_playerPortrait = hud.playerPortrait;
    m_context = HudContextAction::None;
    m_contextGuid = 0;
    m_contextInRange = true;
    m_travel.prepareHud(session, hud); // the minimap's zone name (offline too)
    m_worldStateScreens.prepareHud(hud); // phase 17 (scores): the battle map's arrow (offline too)
    m_minimap.prepareHud(session, hud, m_settings, nowMs); // parity pass: the minimap cluster (offline too)
    m_units.prepareHud(session, hud, nowMs); // parity: unit frames, experience bar, a new target's tooltip
    m_targeting.prepareHud(session, hud, nowMs); // phase 13 (spells): a place or an item to choose (offline: the demo)
    m_unitMenus.prepareHud(session, hud, nowMs); // phase 13 (social): the follow, the lines of /played, /pvp
    m_touch.setOptions(m_settings.touchControls, m_settings.touchLeftHanded); // phase 20
    m_touch.assists().setOptions(m_settings);                                 // phase 20 (assists)
    if (!session.inWorld()) {
        m_touch.prepareOffline(hud, nowMs); // phase 20: VD_TOUCH_DEMO
        return;
    }
    game::Gameplay& play = session.play();
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    if (!me) {
        return;
    }
    const game::Reactions* reactions = hud.reactions;
    const u64 targetGuid = play.target();
    const Entity* target = targetGuid ? entities.find(targetGuid) : nullptr;
    const game::SpellBook& spells = play.spells();
    m_nowMs = nowMs;
    if (m_trace) { // the player's sheath state: when its weapons go to or from its hands
        if (const u8 sheath = play.sheath(); sheath != m_tracedSheath) {
            VD_LOG_INFO("Sheath %u -> %u (server %u)", m_tracedSheath, sheath,
                        me->byteValue(uf::UNIT_FIELD_BYTES_2, 0));
            m_tracedSheath = sheath;
        }
    }

    // Selecting another unit while auto attacking switches the attack to it when it can be attacked, and stops it
    // otherwise (the original client's rule). A tap on empty ground (no target) keeps the fight going.
    if (targetGuid != m_lastTarget) {
        m_lastTarget = targetGuid;
        if (targetGuid != 0 && play.melee().attacking() && play.melee().target() != targetGuid) {
            if (target && game::isUnit(*target) && !game::isDead(*target) &&
                game::canAttack(*me, *target, reactions)) {
                play.startAttack(targetGuid);
            } else {
                play.stopAttack();
            }
        }
    }

    // Facing: the auto attack's victim, or what an attack / spell was just asked against.
    if (hud.player) {
        u64 face = play.melee().attacking() ? play.melee().target() : 0;
        if (face == 0 && m_faceGuid != 0 && static_cast<i32>(m_faceUntilMs - nowMs) > 0) {
            face = m_faceGuid;
        }
        if (const Entity* other = face != 0 && face != session.playerGuid() ? entities.find(face) : nullptr) {
            const glm::vec3 to = other->positionAt(nowMs) - glm::vec3(hud.position);
            if (to.x * to.x + to.y * to.y > 0.01f) {
                hud.player->setFacingGoal(std::atan2(to.y, to.x));
            }
        }
    }

    // The action bar: pages of six buttons, their edit mode (phase 8, CombatScreens).
    m_combat.prepareActionBar(session, hud, nowMs);
    // Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster instead of the bars (hud.touch), unless the classic ones.
    m_touch.prepareHud(session, hud, m_combat, nowMs);

    // phase 20 (smart): the context button's new subjects (resurrect, skin...) here
    m_touch.smart().prepareHud(session, hud, nowMs); // the next enemy's rule (the Target button)
    if (const TouchSmart::Subject s = m_touch.smart().subject(session, nowMs); s.action != HudContextAction::None) {
        m_context = s.action; // a dead friendly player, a looted skinnable corpse: the target, as below
        m_contextInRange = s.inRange;
        hud.contextActionIcon = s.icon;
    }
    // The context button: what a tap on the target (or the object at hand) does.
    if (target && game::isUnit(*target)) {
        const bool dead = game::isDead(*target);
        // The server measures between the bounding spheres (a spirit healer is several yards wide).
        const f32 gap = game::distanceAt(*me, *target, nowMs) - floatValue(*target, uf::UNIT_FIELD_BOUNDINGRADIUS) -
                        floatValue(*me, uf::UNIT_FIELD_BOUNDINGRADIUS);
        const bool near = gap <= game::kInteractionDistance;
        if (dead && game::isLootable(*target)) {
            m_context = HudContextAction::Loot;
            m_contextInRange = near;
        } else if (!dead && game::canAttack(*me, *target, reactions)) {
            m_context = play.melee().attacking() && play.melee().target() == targetGuid ? HudContextAction::StopAttack
                                                                                          : HudContextAction::Attack;
        } else if (!dead && (game::npcFlags(*target) & (game::kNpcFlagGossip | game::kNpcFlagQuestGiver |
                                                        game::kNpcFlagSpiritHealer | game::kNpcFlagTrainer |
                                                        game::kNpcFlagVendor | game::kNpcFlagBanker |
                                                        game::kNpcFlagFlightMaster |
                                                        game::kNpcFlagAuctioneer)) != 0 &&
                   game::reactionTo(*me, *target, reactions) != game::Reaction::Hostile) {
            m_context = HudContextAction::Talk;
            m_contextInRange = near;
        }
        m_contextGuid = m_context != HudContextAction::None ? targetGuid : 0;
    }
    if (m_context == HudContextAction::None) { // ours: a corpse at hand loots without selecting it first
        if (const u64 corpse = nearbyLoot(session)) {
            m_context = HudContextAction::Loot;
            m_contextGuid = corpse;
        }
    }
    if (m_context == HudContextAction::None) {
        if (const u64 object = nearbyObject(session)) {
            m_context = HudContextAction::Use;
            m_contextGuid = object;
            const Entity* go = entities.find(object);
            m_contextInRange = go && game::distanceAt(*me, *go, nowMs) <= kObjectUseYards;
        }
    }
    if (m_context == HudContextAction::None) { // phase 20 (smart): a herb or a vein the player can gather (§6.4)
        if (const Entity* node = entities.find(m_touch.smart().gatherNode(session, nowMs))) {
            m_context = HudContextAction::Use;
            m_contextGuid = node->guid;
            m_contextInRange = game::distanceAt(*me, *node, nowMs) <= kObjectUseYards;
        }
    }
    hud.context = m_context;
    hud.contextInRange = m_contextInRange;
    // Parity: the button shows its subject's name and the client's cursor over it.
    if (m_context != HudContextAction::None) {
        hud.contextName = objectName(session, m_contextGuid);
        hud.contextIcon = contextCursor(session, m_data, m_context, m_contextGuid);
    }

    // The cast bar, and its flash when the cast ends (green done, red interrupted).
    const game::CastBar& cast = spells.cast();
    if (cast.active()) {
        if (cast.spellId != m_lastCastSpell) {
            m_lastCastSpell = cast.spellId;
            const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(cast.spellId) : std::nullopt;
            m_castName.assign(spell ? spell->name : std::string_view("..."));
        }
        m_castFailed = false;
        m_castEndMs = 0;
        hud.cast = {true, m_castName, cast.progress(nowMs), cast.channel};
    } else if (m_lastCastSpell != 0) {
        m_castEndMs = nowMs;
        m_lastCastSpell = 0;
    }
    if (!cast.active() && m_castEndMs != 0 && nowMs - m_castEndMs < 500) {
        hud.cast = {true, m_castName, 1.0f, false, m_castFailed, !m_castFailed};
    }

    // Auras: the player's with their time left and its weapons' enchantments, the target's (app/AuraScreens).
    m_auras.prepareHud(session, hud, nowMs);
    // Phase 15 (options): the target of target only as SHOW_TARGET_OF_TARGET says (its frame narrows the debuff rows).
    const game::GroupState& group = play.social().group();
    hud.targetOfTarget = hud.targetOfTarget && OptionsScreens::targetOfTargetShown(m_settings, group.active, group.raid);
}

void GameScreens::applyOptions(GameSession& session) {
    m_combat.setLockActionBars(m_settings.lockActionBars);
    m_inventory.setAlwaysCompare(m_settings.alwaysCompare); // ours: the item tooltips' comparisons
    m_combat.setShowPetBar(!m_settings.touchControls || m_settings.touchPetBar); // phase 20 (pets)
    m_unitMenus.setAssistAttack(m_settings.assistAttack); // ASSIST_ATTACK
    m_social.setPartyOptions(m_settings.hidePartyInterface, m_settings.showPartyBackground);
    m_social.setShowPartyPets(m_settings.showPartyPets);       // SHOW_PARTY_PETS (f15/pets' PartyMemberPetFrame)
    m_raidPullouts.setFilters(m_settings.showDispellableDebuffs, m_settings.showCastableBuffs); // phase 16 (pullouts)
    m_social.setGuildMemberAlert(m_settings.guildMemberAlert); // GUILDMEMBER_ALERT
    m_exchange.setShowLootSpam(m_settings.showLootSpam);
    m_units.setNewbieTips(m_settings.showNewbieTips);
    if (!session.inWorld()) {
        return;
    }
    game::Gameplay& play = session.play();
    // CLEAR_AFK ("autoClearAFK"): moving or talking ends AFK (the empty CHAT_MSG_AFK toggles it), once until the
    // server's PLAYER_FLAGS say so.
    game::Social& social = play.social();
    const bool spoke = m_social.takeSpoke();
    if (!social.afk()) {
        m_afkClearSent = false;
    } else if (m_settings.autoClearAfk && (spoke || m_playerMoving) && !m_afkClearSent) {
        social.setAfk({});
        m_afkClearSent = true;
    }
    play.autoSelfCast = m_settings.autoSelfCast;            // AUTO_SELF_CAST
    play.exchange().trade().blockRequests = m_settings.blockTrades; // BLOCK_TRADES
    const game::GroupState& group = play.social().group();
    m_combat.setTargetOfTargetShown(OptionsScreens::targetOfTargetShown(m_settings, group.active, group.raid));
}

u64 GameScreens::nearbyObject(GameSession& session) {
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    if (!me) {
        return 0;
    }
    u64 best = 0;
    f32 bestDistance = kObjectSearchYards;
    entities.forEach([&](const Entity& e) {
        if (e.type != game::ObjectType::GameObject) {
            return;
        }
        const f32 d = game::distanceAt(*me, e, m_nowMs);
        if (d >= bestDistance) {
            return;
        }
        const u32 type = game::gameObjectType(e);
        const u32 flags = e.value(uf::GAMEOBJECT_FLAGS);
        const bool active = (e.value(uf::GAMEOBJECT_DYN_FLAGS) & game::kGameObjectDynActivate) != 0;
        // Chests and quest objects the player may use now (the server sets the activate flag for quest ones).
        const bool usable = (type == game::kGameObjectChest && active) || type == game::kGameObjectQuestGiver ||
                            (type == game::kGameObjectGoober && active) ||
                            type == game::kGameObjectMailbox; // phase 8: the mailbox
        if (usable && (flags & game::kGameObjectFlagNoInteract) == 0) {
            best = e.guid;
            bestDistance = d;
        }
    });
    if (const Entity* go = best ? entities.find(best) : nullptr) {
        session.gameObject(go->value(uf::OBJECT_FIELD_ENTRY)); // its lock is known by the time it is tapped
    }
    return best;
}

u64 GameScreens::nearbyLoot(GameSession& session) {
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    if (!me) {
        return 0;
    }
    const f32 myRadius = floatValue(*me, uf::UNIT_FIELD_BOUNDINGRADIUS);
    u64 best = 0;
    f32 bestGap = game::kInteractionDistance;
    entities.forEach([&](const Entity& e) {
        if (e.guid == me->guid || !game::isUnit(e) || !game::isDead(e) || !game::isLootable(e)) {
            return;
        }
        // The server measures between the bounding spheres, as for the target.
        const f32 gap = game::distanceAt(*me, e, m_nowMs) - floatValue(e, uf::UNIT_FIELD_BOUNDINGRADIUS) - myRadius;
        if (gap <= bestGap) {
            best = e.guid;
            bestGap = gap;
        }
    });
    return best;
}

void GameScreens::actOnHud(GameSession& session, const HudOutput& out) {
    game::Gameplay& play = session.play();
    m_units.actOnHud(session, out, m_nowMs); // parity: target yourself, the player's menu, kept tooltips
    m_objectTooltip.actOnHud(session, out);  // phase 16 (alerts): a game object held: its tooltip
    if (const u64 aim = m_combat.actOnActionBar(session, out); aim != 0) {
        m_faceGuid = aim; // spells want their target in front
        m_faceUntilMs = m_nowMs + 800;
    }
    if (const u64 aim = m_touch.actOnHud(session, out, m_combat, m_nowMs); aim != 0) { // phase 20
        m_faceGuid = aim;
        m_faceUntilMs = m_nowMs + 800;
    }
    // Parity: the client's right click. A tap on a game object in the world uses it like the button's Use
    // (the button's subject for this frame); a tap on the selected unit does what the button does with it.
    bool objectTapped = false;
    // Phase 13 (spells): a locked object is what Pick Lock waits for.
    const bool objectAimed = m_targeting.actOnObject(session, out.objectTapped);
    if (const Entity* go = out.objectTapped != 0 && !objectAimed ? session.entities().find(out.objectTapped)
                                                                : nullptr) {
        const Entity* me = session.entities().find(session.playerGuid());
        m_context = HudContextAction::Use;
        m_contextGuid = out.objectTapped;
        m_contextInRange = // phase 13: the fishing bobber from where it landed
            me && game::distanceAt(*me, *go, m_nowMs) <= game::gameObjectUseYards(*go, kObjectUseYards);
        session.gameObject(go->value(uf::OBJECT_FIELD_ENTRY)); // its lock: known by the next tap if not yet
        objectTapped = true;
    }
    const bool context = out.context || objectTapped || tapRunsContext(out.unitTapped, m_contextGuid);
    // Ours (NPC voices): the left click on a unit, and the right click on one the context button has nothing for.
    if (const u64 clicked = out.unitSelected != 0 ? out.unitSelected : context ? 0 : out.unitTapped; clicked != 0) {
        play.unitClicked(clicked);
    }
    if (m_trace && (objectTapped || out.unitTapped != 0)) {
        VD_LOG_INFO("Tap on %s %016llx: action %u on %016llx, %s", objectTapped ? "object" : "unit",
                    static_cast<unsigned long long>(objectTapped ? out.objectTapped : out.unitTapped),
                    static_cast<u32>(context ? m_context : HudContextAction::None),
                    static_cast<unsigned long long>(m_contextGuid), m_contextInRange ? "in range" : "too far");
    }
    if (context && !m_contextInRange) {
        message(kErrorColor, false, "ERR_USE_TOO_FAR");
    } else if (context) {
        switch (m_context) {
        case HudContextAction::Attack:
            play.startAttack(m_contextGuid);
            m_faceGuid = m_contextGuid;
            m_faceUntilMs = m_nowMs + 1500;
            break;
        case HudContextAction::StopAttack: // a tap keeps fighting (no accidental stop); a long press stops
            m_faceGuid = m_contextGuid;
            m_faceUntilMs = m_nowMs + 800;
            break;
        case HudContextAction::Talk:
        case HudContextAction::Loot:
        case HudContextAction::Use: {
            // A locked chest or quest object opens with the player's "Opening" spell for its lock type (what the
            // client casts on a click); the others are used.
            u32 openSpell = 0;
            if (const Entity* go = session.entities().find(m_contextGuid); go && m_data) {
                const game::GameObjectInfo* info = session.gameObject(go->value(uf::OBJECT_FIELD_ENTRY));
                const u32 type = game::gameObjectType(*go);
                const u32 lockType = info && (type == game::kGameObjectChest || type == game::kGameObjectGoober ||
                                              type == game::kGameObjectQuestGiver)
                                         ? m_data->lockOpenType(info->data[0])
                                         : 0;
                for (const u32 known : lockType != 0 ? play.spells().known() : std::span<const u32>{}) {
                    const std::optional<game::SpellInfo> spell = m_data->spell(known);
                    for (u32 i = 0; spell && i < 3 && openSpell == 0; ++i) {
                        constexpr u32 kEffectOpenLock = 33;
                        if (spell->effects[i] == kEffectOpenLock &&
                            static_cast<u32>(spell->effectMisc[i]) == lockType) {
                            openSpell = known;
                        }
                    }
                    if (openSpell != 0) {
                        break;
                    }
                }
            }
            if (openSpell != 0) {
                play.castSpellOnObject(openSpell, m_contextGuid);
            } else {
                play.interact(m_contextGuid);
            }
            m_faceGuid = m_contextGuid;
            m_faceUntilMs = m_nowMs + 600;
            break;
        }
        case HudContextAction::Resurrect: // phase 20 (smart): the class's spell on the dead player
        case HudContextAction::Skin:      // the Skinning spell on the corpse (the client's right click)
            m_touch.smart().useSubject(session, m_contextGuid);
            m_faceGuid = m_contextGuid;
            m_faceUntilMs = m_nowMs + 600;
            break;
        default: break;
        }
    }
    if (out.contextHold && m_context == HudContextAction::StopAttack) {
        play.stopAttack();
    }
    m_auras.actOnHud(session, out, m_inventory); // parity: an aura's tooltip, a buff cancelled
    if (out.menu) {
        togglePanel(session, UiPanel::GameMenu); // parity pass: the micro menu's Main Menu does it now
    }
}

// --- Events, combat text, chat --------------------------------------------------------------------------------

std::span<char> GameScreens::nextMessage(u32 color, bool big) {
    return m_messages.add(color, big); // stamped when drawn
}

void GameScreens::messageText(u32 color, bool big, std::string_view line) {
    m_messages.add(line, color, big);
}

void GameScreens::readNotices(GameSession& session, u32 nowMs) {
    // What 1.12 writes itself about loot, money, experience, reputation, learned spells, level-ups, skill-ups
    // and quests, where its default interface shows it (game/Notices): the General chat tab (SYSTEM, LOOT,
    // SKILL), the combat log (MONEY, COMBAT_XP_GAIN, COMBAT_FACTION_CHANGE) or the UIErrorsFrame.
    const game::Entity* me = session.entities().find(session.playerGuid());
    m_notices.update(session.play(), me, m_combat.logNames(session), nowMs);
    while (const game::Notice* n = m_notices.lines().next(m_noticeCursor)) {
        const game::NoticeColor rgb = game::noticeColor(n->kind);
        const u32 color = rgba(rgb.r, rgb.g, rgb.b);
        // Phase 18 (p3): the chat's lines by their message group, for the windows that listen to it (the Combat Log's
        // defaults take MONEY, COMBAT_XP_GAIN, COMBAT_FACTION_CHANGE and COMBAT_HONOR_GAIN).
        u8 group = game::kGroupSystem;
        switch (n->kind) {
        case game::NoticeKind::Loot: group = game::kGroupLoot; break;
        case game::NoticeKind::Skill: group = game::kGroupSkill; break;
        case game::NoticeKind::Money: group = game::kGroupMoney; break;
        case game::NoticeKind::XpGain: group = game::kGroupCombatXpGain; break;
        case game::NoticeKind::FactionChange: group = game::kGroupCombatFactionChange; break;
        case game::NoticeKind::HonorGain: group = game::kGroupCombatHonorGain; break;
        default: break;
        }
        switch (game::noticePlace(n->kind)) {
        case game::NoticePlace::General:
        case game::NoticePlace::CombatLog: m_social.systemNotice(color, n->text.data(), group); break;
        case game::NoticePlace::ErrorsFrame: messageText(color, false, n->text.data()); break;
        }
        if (m_trace) {
            VD_LOG_INFO("Notice %u: %s", static_cast<u32>(n->kind), n->text.data());
        }
    }
}

void GameScreens::readEvents(GameSession& session, Hud& hud, u32 nowMs) {
    game::Gameplay& play = session.play();
    // Phase 20 (retouches): world text type 4's colour (0xCE88B0).
    constexpr u32 kExperienceTextColor = rgba(148, 0, 139);
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        using K = game::GameEventKind;
        if (m_trace) {
            VD_LOG_INFO("Event %u: id %u guid %llx values %d %d %d", static_cast<u32>(e->kind), e->id,
                        static_cast<unsigned long long>(e->guid), e->value[0], e->value[1], e->value[2]);
        }
        switch (e->kind) {
        // The melee errors of the client's UIErrorsFrame (SMSG_ATTACKSWING_*).
        case K::SwingNotInRange: message(kErrorColor, false, "ERR_BADATTACKPOS"); break;
        case K::SwingBadFacing: message(kErrorColor, false, "ERR_BADATTACKFACING"); break;
        case K::SwingNotStanding: message(kErrorColor, false, "ERR_CANTATTACK_NOTSTANDING"); break;
        case K::SwingDeadTarget: message(kErrorColor, false, "SPELL_FAILED_TARGETS_DEAD"); break;
        case K::SwingCantAttack: message(kErrorColor, false, "ERR_INVALID_ATTACK_TARGET"); break;
        case K::XpGained:
            // Phase 20 (retouches): besides its line (readNotices), any experience gained (kills, quests,
            // exploration; not a loss) shows "XP: 45" over the player (WoW.exe 0x607260 from 0x625254 and
            // 0x637F56).
            if (e->id == 0) {
                char line[32];
                hud.combatText(session.playerGuid(), game::formatExperienceWorldText(line, e->value[0]),
                               kExperienceTextColor, false, nowMs, HudWorldText::Notice);
            }
            break;
        case K::MountFailed: { // phase 13: SMSG_MOUNTRESULT / DISMOUNTRESULT (UnitMountResult order)
            static constexpr text::Key kMount[] = {
                "ERR_MOUNT_INVALIDMOUNTEE", "ERR_MOUNT_TOOFARAWAY",   "ERR_MOUNT_ALREADYMOUNTED",
                "ERR_MOUNT_NOTMOUNTABLE",   "ERR_MOUNT_NOTYOURPET",   "ERR_MOUNT_OTHER",
                "ERR_MOUNT_LOOTING",        "ERR_MOUNT_RACECANTMOUNT", "ERR_MOUNT_SHAPESHIFTED",
                "ERR_MOUNT_FORCEDDISMOUNT"};
            static constexpr text::Key kDismount[] = {"ERR_DISMOUNT_NOPET", "ERR_DISMOUNT_NOTMOUNTED",
                                                      "ERR_DISMOUNT_NOTYOURPET"};
            const u32 result = static_cast<u32>(e->value[0]);
            if (e->value[1] == 0 && result < std::size(kMount)) {
                message(kErrorColor, false, kMount[result]);
            } else if (e->value[1] != 0 && result < std::size(kDismount)) {
                message(kErrorColor, false, kDismount[result]);
            }
            break;
        }
        case K::CastFailed: {
            const std::string_view name = net::spellCastResultName(static_cast<u8>(e->value[0]));
            if (name != "DONT_REPORT") {
                castFailedText(nextMessage(kErrorColor, false), session, m_data, e->id, name, e->value[1]);
            }
            if (e->id == m_lastCastSpell || m_castEndMs != 0) {
                m_castFailed = true;
            }
            break;
        }
        case K::SpellLearned:
            // Phase 13: 1.12 never puts a learned spell on the action bar (SMSG_LEARNED_SPELL only fires
            // LEARNED_SPELL_IN_TAB: SpellBookFrame.lua flashes the spell book's tab); the player drags it from the
            // spell book. Learning a profession taught a dozen recipes, which used to fill the main bar.
            break; // its line goes to the chat (readNotices), as experience, level-ups, loot and money do
        case K::LootError: {
            // ERR_LOOT_<LootError>, or ERR_<it> for the ones without the prefix (ALREADY_PICKPOCKETED...).
            const std::string_view name = net::lootErrorName(static_cast<u8>(e->value[0]));
            char keyName[64];
            std::snprintf(keyName, sizeof(keyName), "ERR_LOOT_%.*s", static_cast<int>(name.size()), name.data());
            if (!text::has(std::string_view(keyName))) {
                std::snprintf(keyName, sizeof(keyName), "ERR_%.*s", static_cast<int>(name.size()), name.data());
            }
            message(kErrorColor, false, text::has(std::string_view(keyName)) ? text::Key(std::string_view(keyName))
                                                                             : text::Key("ERR_LOOT_NO_UI"));
            break;
        }
        // Items, vendors, the bank (phase 8: InventoryScreens has their texts).
        case K::InventoryError:
        case K::BuyFailed:
        case K::SellFailed:
        case K::BankSlotFailed:
        case K::BankSlotBought:
        case K::ReadItemFailed:
        case K::DurabilityLoss: {
            char line[160];
            u32 color = kErrorColor;
            if (m_inventory.eventMessage(session, *e, line, sizeof(line), color)) {
                messageText(color, false, line);
            }
            break;
        }
        // Quests accepted and turned in are chat lines (readNotices); objectives done and failures are
        // UIErrorsFrame ones.
        case K::QuestObjectivesDone:
        case K::QuestFailed: {
            const game::QuestInfo* quest = play.quest(e->id);
            const std::string_view title = quest                    ? std::string_view(quest->title)
                                           : e->id == m_dialogQuest ? std::string_view(m_dialogQuestTitle)
                                                                    : std::string_view("...");
            const bool done = e->kind == K::QuestObjectivesDone;
            message(done ? kInfoColor : kErrorColor, false,
                    done ? "ERR_QUEST_OBJECTIVE_COMPLETE_S" : "ERR_QUEST_FAILED_S", title);
            break;
        }
        case K::QuestObjectiveProgress: {
            // "Kobold Vermin slain: 3/10", as the client's UIErrorsFrame.
            std::string_view name;
            const u32 entry = static_cast<u32>(e->value[0]);
            if ((entry & game::kQuestObjectiveGameObject) == 0) {
                if (const net::CreatureQueryInfo* creature = session.creature(entry)) {
                    name = creature->name;
                }
            } else if (const game::GameObjectInfo* object = session.gameObject(entry & ~game::kQuestObjectiveGameObject)) {
                name = object->name;
            }
            const bool kill = (entry & game::kQuestObjectiveGameObject) == 0;
            message(kInfoColor, false, kill ? "ERR_QUEST_ADD_KILL_SII" : "ERR_QUEST_ADD_FOUND_SII", name, e->value[1],
                    e->value[2]);
            break;
        }
        case K::QuestItemProgress: {
            // "Linen Cloth: 3/8", the counts as game::Gameplay worked them out (WoW.exe 0x5DD060).
            const net::ItemQueryInfo* info = session.item(e->id);
            if (info && e->value[2] > 0) {
                message(kInfoColor, false, "ERR_QUEST_ADD_ITEM_SII", info->name, e->value[1], e->value[2]);
            }
            break;
        }
        case K::QuestLogFull: message(kErrorColor, false, "ERR_QUEST_LOG_FULL"); break;
        case K::QuestInvalid:
        case K::QuestGiverFailed: {
            const game::QuestInfo* quest = e->kind == K::QuestGiverFailed ? play.quest(e->id) : nullptr;
            questFailedText(nextMessage(kErrorColor, false), static_cast<u32>(e->value[0]),
                            quest ? std::string_view(quest->title) : std::string_view{});
            break;
        }
        // Shared quests, resurrections, the spirit healer: app/PopupScreens (parity: StaticPopups).
        case K::PlayerDied: // PLAYER_DEAD: CloseAllWindows(1) (syncPanels), then the DEATH popup
            m_inventory.closeAll();
            play.closeLoot();
            play.closeDialog();
            break;
        case K::ControlLost: // phase 13: PLAYER_CONTROL_LOST: CloseAllWindows_WithExceptions, not on a taxi
            if (!play.travel().flying()) {
                // The options stay (IsOptionFrameOpen); phase 17 (scores): the score too (the battle's end stuns).
                m_panels.closeAll(m_panels.shown(UiPanel::Options) ||
                                  m_panels.center() == UiPanel::WorldStateScore);
                m_inventory.closeAll();
                play.closeLoot();
                play.closeDialog();
            }
            break;
        case K::FeignDeathResisted: message(kErrorColor, false, "ERR_FEIGN_DEATH_RESISTED"); break; // phase 13
        case K::ContainerOpened: m_inventory.containerOpened(e->guid, nowMs); break; // phase 15 (items): BAG_OPEN
        case K::TradeBlocked: { // phase 15 (options): BLOCK_TRADES refused it (WoW.exe's game error 187)
            const std::string* name = session.playerName(e->guid);
            message(kErrorColor, false, "ERR_TRADE_BLOCKED_S", name ? std::string_view(*name) : std::string_view{});
            break;
        }
        default: break;
        }
    }
}

void GameScreens::readCombat(GameSession& session, Hud& hud, u32 nowMs) {
    game::Gameplay& play = session.play();
    const u64 me = session.playerGuid();
    while (const game::CombatEvent* e = play.combatLog().next(m_combatCursor)) {
        if (m_trace) {
            VD_LOG_INFO("Combat %u: %llx -> %llx spell %u amount %d flags %x", static_cast<u32>(e->kind),
                        static_cast<unsigned long long>(e->attacker), static_cast<unsigned long long>(e->victim),
                        e->spellId, e->amount, e->flags);
        }
        // Only what the player does or suffers (and heals it gets).
        const u64 pet = play.abilities().pet().guid;
        const bool byPet = pet != 0 && e->attacker == pet && e->victim != me;
        if (e->attacker != me && e->victim != me && !byPet) {
            continue;
        }
        const bool mine = e->victim == me;
        // Phase 15 (options): SHOW_DAMAGE_TEXT (CombatDamage) shows the player's blows over its victims,
        // LOG_PERIODIC_EFFECTS (CombatLogPeriodicSpells) its periodic damage too, SHOW_PET_MELEE_DAMAGE (PetMeleeDamage)
        // its pet's.
        if (!mine && e->kind != game::CombatEvent::Kind::Heal) {
            if (!m_settings.showDamage || (byPet && !m_settings.showPetDamage) ||
                ((e->flags & game::CombatEvent::kPeriodic) != 0 && !m_settings.logPeriodicEffects)) {
                continue;
            }
        }
        const bool crit = (e->flags & game::CombatEvent::kCrit) != 0;
        char label[24];
        u32 color = mine ? rgba(255, 64, 64) : ui::kWhite;
        using K = game::CombatEvent::Kind;
        switch (e->kind) {
        case K::Damage:
            std::snprintf(label, sizeof(label), crit ? "%d!" : "%d", e->amount);
            if (!mine && e->spellId != 0) {
                color = rgba(255, 255, 0);
            }
            break;
        case K::Heal:
            std::snprintf(label, sizeof(label), "+%d", e->amount);
            color = rgba(64, 255, 64);
            break;
        // The floating combat text's words (CombatText.lua).
        case K::Miss: text::format(label, "MISS"); break;
        case K::Dodge: text::format(label, "DODGE"); break;
        case K::Parry: text::format(label, "PARRY"); break;
        case K::Block: text::format(label, "BLOCK"); break;
        case K::Evade: text::format(label, "EVADE"); break;
        case K::Immune: text::format(label, "IMMUNE"); break;
        case K::Resist: text::format(label, "RESIST"); break;
        case K::Absorb: text::format(label, "ABSORB"); break;
        default: continue;
        }
        hud.combatText(e->victim, label, color, crit, nowMs);
    }
}

void GameScreens::drawMessages(Hud& hud, const ui::Rect& area, u64 nowMs, f32 below, const ui::Rect& avoid) {
    // The client's UIErrorsFrame (app/MessageHud): its newest line on top, each shown 5 s, then a 3 s fade.
    DialogContext c = context(hud, area);
    m_messages.update(nowMs);
    errorsFrame(c, area, m_messages, nowMs, below, avoid);
}

// --- Windows --------------------------------------------------------------------------------------------------

ItemView GameScreens::item(GameSession& session, u32 entry, u32 count) {
    ItemView view;
    view.count = count;
    view.entry = entry;
    if (const net::ItemQueryInfo* info = session.item(entry)) {
        view.name = info->name;
        view.quality = info->quality;
        view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
        view.usable = session.play().inventory().usable(*info); // GetQuestItemInfo's isUsable (rewards only)
    } else {
        view.name = "...";
    }
    return view;
}

std::string_view GameScreens::objectName(GameSession& session, u64 guid) {
    const Entity* e = session.entities().find(guid);
    if (!e) {
        return {};
    }
    const u32 entry = e->value(uf::OBJECT_FIELD_ENTRY);
    if (e->type == game::ObjectType::GameObject) {
        const game::GameObjectInfo* info = session.gameObject(entry);
        return info ? std::string_view(info->name) : std::string_view{};
    }
    if (e->type == game::ObjectType::Player) {
        const std::string* name = session.playerName(guid);
        return name ? std::string_view(*name) : std::string_view{};
    }
    const net::CreatureQueryInfo* creature = session.creature(entry);
    return creature ? std::string_view(creature->name) : std::string_view{};
}

std::string_view GameScreens::expand(GameSession& session, std::string_view text, u32 buffer) {
    game::TextSubject subject;
    if (const Entity* me = session.entities().find(session.playerGuid())) {
        subject.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
        subject.cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        subject.gender = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    }
    if (const std::string* name = session.playerName(session.playerGuid())) {
        subject.name = *name;
    }
    std::string& out = m_expanded[buffer % m_expanded.size()];
    game::expandText(text, subject, out);
    return out;
}

void GameScreens::npcWindow(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    const game::NpcDialog& dialog = play.dialog();
    const std::string_view npcName = objectName(session, dialog.npc());
    const game::QuestDialog& quest = dialog.quest();
    // Retouches (quest windows): QuestFrame_SetPortrait: a unit's portrait (SetPortraitTexture), an object's book.
    NpcPortraitView npc;
    if (const Entity* e = session.entities().find(dialog.npc())) {
        npc.unit = e->type == game::ObjectType::Unit || e->type == game::ObjectType::Player;
    }
    if (npc.unit && m_portraits) {
        npc.portrait = m_portraits->unit(dialog.npc());
    }
    const auto scrolled = [&](bool scroll) {
        if (scroll) {
            m_uiSound(UiSound::ChatScroll); // UIPanelScrollBarTemplate's buttons: PlaySound("UChatScrollButton")
        }
    };
    if (quest.questId != 0 && quest.questId != m_dialogQuest) {
        m_dialogQuest = quest.questId;
        m_dialogQuestTitle.assign(quest.title);
    }
    switch (dialog.kind()) {
    case game::DialogKind::Gossip:
    case game::DialogKind::QuestList: {
        GossipView view;
        view.npcName = npcName;
        if (dialog.kind() == game::DialogKind::QuestList) {
            view.text = expand(session, dialog.greeting(), 0);
        } else if (const game::NpcText* text = play.npcText(dialog.textId())) {
            const Entity* me = session.entities().find(session.playerGuid());
            view.text = expand(session, text->text(me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) : 0), 0);
            view.text = play.gossipText(text->language(), view.text, m_gossipHeard); // phase 18 (extras): 0x4E22AB
        }
        m_gossipOptions.clear();
        for (const game::GossipOption& o : dialog.options()) {
            m_gossipOptions.push_back({o.text, o.icon});
        }
        m_gossipQuests.clear();
        for (const game::QuestMenuEntry& q : dialog.quests()) {
            const bool turnIn = game::turnsIn(q.icon);
            const bool done = q.icon == net::QuestGiverStatus::Reward || q.icon == net::QuestGiverStatus::RewardRep ||
                              q.icon == net::QuestGiverStatus::RewardOld;
            m_gossipQuests.push_back({q.title, q.level, turnIn, done});
        }
        view.options = m_gossipOptions;
        view.quests = m_gossipQuests;
        view.npc = npc;
        const GossipResult r = gossipDialog(c, view);
        scrolled(r.scrolled);
        if (r.option >= 0) {
            play.selectGossipOption(dialog.options()[static_cast<u32>(r.option)].index);
        } else if (r.quest >= 0) {
            play.selectQuest(dialog.quests()[static_cast<u32>(r.quest)].questId);
        } else if (r.closed) {
            play.closeDialog();
        }
        break;
    }
    case game::DialogKind::QuestDetails: {
        m_items.clear();
        m_choiceItems.clear();
        for (u32 i = 0; i < quest.rewardCount; ++i) {
            m_items.push_back(item(session, quest.rewards[i].itemId, quest.rewards[i].count));
        }
        for (u32 i = 0; i < quest.choiceCount; ++i) {
            m_choiceItems.push_back(item(session, quest.choices[i].itemId, quest.choices[i].count));
        }
        QuestDetailsView view{npcName, quest.title, expand(session, quest.text, 0), expand(session, quest.objectives, 1),
                              m_items, m_choiceItems, static_cast<u32>(std::max(0, quest.money)), 0, npc};
        const QuestDetailsResult r = questDetailsDialog(c, view);
        scrolled(r.scrolled);
        if (r.accept) {
            play.acceptQuest();
        } else if (r.decline) {
            play.closeDialog();
        }
        break;
    }
    case game::DialogKind::QuestRequestItems: {
        m_items.clear();
        for (u32 i = 0; i < quest.requiredCount; ++i) {
            m_items.push_back(item(session, quest.required[i].itemId, quest.required[i].count));
        }
        const QuestProgressView view{npcName, quest.title, expand(session, quest.text, 0), m_items, quest.requiredMoney,
                                     quest.completable, npc};
        const QuestProgressResult r = questProgressDialog(c, view);
        scrolled(r.scrolled);
        if (r.proceed) {
            play.completeQuest();
        } else if (r.closed) {
            play.closeDialog();
        }
        break;
    }
    case game::DialogKind::QuestOfferReward: {
        if (m_rewardQuest != quest.questId) {
            m_rewardQuest = quest.questId;
            m_rewardChoice = -1;
        }
        m_items.clear();
        m_choiceItems.clear();
        for (u32 i = 0; i < quest.rewardCount; ++i) {
            m_items.push_back(item(session, quest.rewards[i].itemId, quest.rewards[i].count));
        }
        for (u32 i = 0; i < quest.choiceCount; ++i) {
            m_choiceItems.push_back(item(session, quest.choices[i].itemId, quest.choices[i].count));
        }
        const QuestRewardView view{npcName, quest.title, expand(session, quest.text, 0), m_items, m_choiceItems,
                                   static_cast<u32>(std::max(0, quest.money)), 0, npc};
        const QuestRewardResult r = questRewardDialog(c, view, m_rewardChoice);
        scrolled(r.scrolled);
        if (r.complete) {
            play.chooseQuestReward(r.choice >= 0 ? static_cast<u32>(r.choice) : 0);
        } else if (r.closed) {
            play.closeDialog();
        }
        break;
    }
    default: break;
    }
}

void GameScreens::lootWindow(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    const game::LootWindow& loot = play.loot();
    if (loot.empty()) {
        play.closeLoot(); // everything taken: the client lets go of the corpse by itself
        return;
    }
    m_lootItems.clear();
    for (const game::LootSlot& slot : loot.items()) {
        if (!slot.taken) {
            m_lootItems.push_back({item(session, slot.itemId, slot.count), slot.slot,
                                   m_exchange.lootNote(session, slot.slotType)}); // phase 8: group loot
        }
    }
    if (loot.guid() != m_lootGuid) { // LOOT_OPENED: back to the first page
        m_lootGuid = loot.guid();
        m_lootPage = 0;
        m_autoLootNext = m_settings.autoLoot ? 0u : kAutoLootDone;
        m_autoLootAtMs = m_nowMs + kAutoLootFirstMs;
        m_autoLootAsked = false;
    }
    const LootView view{objectName(session, loot.guid()), m_lootItems, loot.gold(), m_lootPage};
    const LootResult r = vd::lootWindow(c, view);
    if (r.page >= 0) {
        m_lootPage = static_cast<u32>(r.page);
    }
    if (r.take >= 0) {
        const LootItemView& taken = m_lootItems[static_cast<u32>(r.take)];
        const u8 slot = static_cast<u8>(taken.slot);
        game::ItemInstance link; // phase 13 (chat links): LootFrameItem_OnClick's Shift + click (GetLootSlotLink)
        link.entry = taken.item.entry;
        for (const game::LootSlot& s : loot.items()) {
            link.randomPropertyId = s.slot == slot ? s.randomPropertyId : link.randomPropertyId;
        }
        // Phase 8: rolls, the master looter. Parity (popups): bind on pickup asks first (LOOT_BIND).
        if (m_inventory.insertItemLink(session, c, link)) {
            // the chat was open: linked, not taken
        } else if (!m_exchange.lootTapped(session, slot, r.takeRect) &&
            !m_popups.askBindForLoot(session, slot, taken.item.entry)) {
            play.lootItem(slot);
        }
    }
    if (r.takeMoney) {
        play.lootMoney();
    }
    if (r.closed) {
        play.closeLoot();
    } else {
        autoLoot(session);
    }
}

void GameScreens::autoLoot(GameSession& session) {
    // Auto Loot (ours: 1.12 only has Shift + click): the money, then the items in their order, one a step. Parity
    // (popups): what binds when picked up waits for its LOOT_BIND (the first one asks); what the group rolls for or the
    // master looter gives stays, and so does what the bags have no room for.
    if (m_autoLootNext == kAutoLootDone || static_cast<i32>(m_nowMs - m_autoLootAtMs) < 0) {
        return;
    }
    game::Gameplay& play = session.play();
    const std::span<const game::LootSlot> items = play.loot().items();
    bool took = false;
    if (m_autoLootNext == 0) {
        m_autoLootNext = 1;
        took = play.loot().gold() > 0;
        play.lootMoney();
    }
    while (!took && m_autoLootNext - 1 < items.size()) {
        const game::LootSlot& slot = items[m_autoLootNext++ - 1];
        if (!slot.lootable()) {
            continue;
        }
        if (game::lootBindAsk(play, slot.itemId) == game::BindAsk::None) {
            play.lootItem(slot.slot);
            took = true;
        } else if (!m_autoLootAsked) {
            m_autoLootAsked = m_popups.askBindForLoot(session, slot.slot, slot.itemId);
        }
    }
    m_autoLootNext = took ? m_autoLootNext : kAutoLootDone;
    m_autoLootAtMs = m_nowMs + kAutoLootStepMs;
}

void GameScreens::deathHint(GameSession& session, DialogContext& c, u32 nowMs) {
    // A ghost on its way back to the corpse (RECOVER_CORPSE is a popup once it is there: app/PopupScreens).
    game::Gameplay& play = session.play();
    const game::DeathState& death = play.death();
    // Phase 17: a battleground's spirit guide says the time itself (AREA_SPIRIT_HEAL).
    if (!death.dead || !death.ghost || m_popups.stack().visible(PopupKind::RecoverCorpse) ||
        m_popups.stack().visible(PopupKind::AreaSpiritHeal)) {
        return;
    }
    if (death.corpseKnown) {
        ui::Gui g{c.ui, c.draw};
        const Entity* me = session.entities().find(session.playerGuid());
        char hint[128];
        const u32 wait = play.reclaimDelayRemaining(nowMs);
        if (me && wait == 0) {
            const glm::vec3 at = me->positionAt(nowMs);
            const f32 dx = at.x - death.corpseX, dy = at.y - death.corpseY;
            text::format(hint, "VD_DEATH_CORPSE_DISTANCE", std::sqrt(dx * dx + dy * dy));
        } else {
            text::format(hint, "RECOVER_CORPSE_TIMER", (wait + 999) / 1000, text::get("SECONDS_ABBR"));
        }
        ui::TextStyle style = ui::gameFont(g.px(16.0f), rgba(200, 200, 255));
        style.outline = std::max(1.0f, g.px(1.0f));
        style.align = ui::Align::Center;
        g.draw.text(c.safeArea.centerX(), c.safeArea.y + g.unit(110.0f), hint, style);
    }
}

QuestScreens::Frame GameScreens::questFrame(GameSession& session, u32 nowMs) {
    // Phase 13 (quests): the server's clock for the timed quests, AUTO_QUEST_WATCH from the settings.
    return {session.play(), session.entities(), session.playerGuid(), session.serverUnixTime(),
            m_settings.autoQuestWatch, nowMs};
}

void GameScreens::drawGame(GameSession& session, Hud& hud, ui::TextInput& input, const ui::Rect& area, u32 nowMs) {
    VD_PROFILE_ZONE("GameScreens::drawGame");
    m_chatLink.bind(&input);
    m_touch.assists().setHudFrames(hud.playerFrameRect(), hud.targetFrameRect(), hud.actionBarRect()); // 20 (assists)
    if (!session.inWorld()) {
        DialogContext offline = context(hud, area);
        m_touch.actOffline(hud.output()); // phase 20: VD_TOUCH_DEMO's fan
        m_touch.drawOffline(offline);     // phase 20 (smart): VD_TOUCH_SMART_DEMO's lists
        m_travel.drawOffline(session, offline, nowMs); // the zone text works without a server (development)
        m_targeting.drawWorld(session, offline, hud.output()); // VD_TARGETING_DEMO (development)
        m_targeting.drawBar(session, offline, hud.actionBarRect());
        m_minimap.drawGame(session, hud.output().minimap, offline, m_settings, nowMs);
        m_battlefield.drawOffline(session, offline, m_minimap.shown(), nowMs); // phase 17 (queue): its demo
        m_units.drawOverlay(session, offline, hud, nowMs); // VD_HUD_DEMO: its sample tooltips
        m_raidAlerts.drawOffline(offline, area, hud.actionBarRect(), nowMs); // phase 16 (alerts): dev switches
        m_pvp.drawOffline(offline, m_combat, m_social, nowMs);                // phase 17 (honor): VD_PVP_DEMO
        if (m_demo) {
            CharacterScreens::drawWatchBarDemo(offline, hud.experienceBarRect()); // phase 15: VD_HUD_DEMO's bar
        }
        if (m_raidPullouts.demo()) { // phase 16 (pullouts): VD_RAID_DEMO's sample raid over the HUD
            const UiPanelPlaces demoPlaces = panelPlaces(hud, offline);
            m_raidPullouts.keepClearOfHud(hud, demoPlaces.stickPad, microMenuLayout(offline).strip, offline);
            m_raidPullouts.draw(session, offline, nowMs);
            m_raidPullouts.drawOverlay(session, offline, nowMs);
        }
        m_cursorsPlayer = 0; // parity (popups): back from the character list, even as the same character
        m_popups.leaveWorld();
        if (!m_demo) {
            m_social.chat(session, offline, input, hud.actionBarRect(), nowMs); // VD_HUD_DEMO: sample lines
            m_touch.drawOffline(offline, input, hud.actionBarRect(), nowMs);     // phase 20 (edit): VD_TOUCH_EDIT_DEMO
            if (m_worldStateScreens.demo()) { // phase 17 (scores): VD_WORLDSTATE_DEMO, VD_SCORE_DEMO
                m_worldStateScreens.drawOffline(session, offline, hud, m_settings, panelPlaces(hud, offline),
                                                nowMs);
            }
            if (m_popupDemoSet != 0) {
            m_popupDemo.draw(offline, m_popupDemoSet - 1, nowMs); // VD_POPUP_DEMO: sample popups over the HUD
            }
            if (m_tutorialDemo) { // phase 18 (p3): VD_P3_TUTORIAL
                m_tutorialScreens.drawAlerts(nullptr, offline, hudBottom(hud, offline), hud.actionBarRect(), nowMs);
                m_tutorialScreens.drawFrame(nullptr, offline, hudBottom(hud, offline), hud.actionBarRect());
            }
            return;
        }
        // VD_HUD_DEMO: the micro menu and the panels with sample data, placed as in the game (app/PanelDemo).
        m_microLayout = microMenuLayout(offline);
        const UiPanelPlaces places = panelPlaces(hud, offline);
        m_panels.setCapacity(places.right.w > 0.0f ? 2 : 1);
        if (m_demoOpen != UiPanel::None) {
            m_panels.show(std::exchange(m_demoOpen, UiPanel::None), false, true);
        }
        m_panels.takeHidden();
        if (!m_panels.any() || !uiPanelOverlaps(m_social.chatRect(), places.both)) {
            m_social.chat(session, offline, input, hud.actionBarRect(), nowMs);
            if (const ui::Rect watch = questWatchPlace(offline, area); watch.w > 0.0f) {
                m_demo->drawQuestWatch(offline, watch); // the sample watched quests over the chat
            }
        }
        // The rim's backpack: every bag; held, the micro menu's bag bar (phase 20).
        const ui::WidgetState backpack = backpackButton(offline);
        if (backpack.clicked) {
            toggleBags(session);
        }
        if (backpack.longPress) {
            m_microState.bagsUnfolded = !m_microState.bagsUnfolded;
        }
        drawPanels(session, offline, input, places, nowMs);
        microMenu(session, offline); // phase 20 (retouches): its rows over the windows
        if (m_worldStateScreens.demo()) { // phase 17 (scores): VD_WORLDSTATE_DEMO, VD_SCORE_DEMO
            m_worldStateScreens.drawOffline(session, offline, hud, m_settings, places, nowMs);
        }
        syncBags(session); // phase 15 (items): the demo's frames closed by their buttons
        if (m_popupDemoSet != 0) {
            m_popupDemo.draw(offline, m_popupDemoSet - 1, nowMs); // VD_POPUP_DEMO: sample popups over the HUD
        }
        if (m_tutorialDemo) { // phase 18 (p3): VD_P3_TUTORIAL
            m_tutorialScreens.drawAlerts(nullptr, offline, hudBottom(hud, offline), hud.actionBarRect(), nowMs);
            m_tutorialScreens.drawFrame(nullptr, offline, hudBottom(hud, offline), hud.actionBarRect());
        }
        drawFullScreenPanel(session, offline, input, nowMs);
        return;
    }
    game::Gameplay& play = session.play();
    if (const Entity* me = session.entities().find(session.playerGuid())) {
        text::setGender(me->byteValue(uf::UNIT_FIELD_BYTES_0, 2)); // phase 21: "$g<male>:<female>;" of the texts
    }
    if (m_cursorsPlayer != session.playerGuid()) {
        // A new character in the world: start reading its events from now.
        m_cursorsPlayer = session.playerGuid();
        m_eventCursor = play.events().end();
        m_combatCursor = play.combatLog().end();
        m_panels.closeAll();
        m_panels.takeHidden();
        m_inventory.closeAll();
        m_notices.restart(play);
        m_noticeCursor = m_notices.lines().end();
        m_quests.restart(play); // phase 13 (quests)
        m_messages.clear();
        m_macros.enterWorld(session); // phase 15 (options): the account's and the character's macros
        m_touch.enterWorld(session);  // phase 20: the character's touch layout
        m_accountFiles.enterWorld(session); // phase 18 (p3): chat-cache.txt, the server's texts
        m_combatText.reset();
        m_objectTooltip.reset(); // phase 16 (alerts)
    }
    actOnHud(session, hud.output());
    // Phase 13 (social): the unit frames' long presses open their menus (the pet frame's from last frame).
    m_unitMenus.actOnHud(session, hud, m_combat.takePetFrameHold(), m_combat.petFrameRect());
    if (m_unitMenus.takeOpenCharacter()) {
        // PET_PAPERDOLL: ToggleCharacter("PetPaperDollFrame"): the window on the Pet tab (phase 15: tab 1), closed
        // when it already shows it.
        if (!m_panels.shown(UiPanel::Character)) {
            togglePanel(session, UiPanel::Character);
            m_inventory.setCharacterTab(1);
        } else if (m_inventory.characterTab() != 1) {
            m_inventory.setCharacterTab(1);
        } else {
            togglePanel(session, UiPanel::Character);
        }
    }
    readEvents(session, hud, nowMs);
    readCombat(session, hud, nowMs);
    m_combatText.update(session, m_settings, nowMs); // phase 15 (options): the floating combat text
    m_social.update(session, nowMs);
    for (SocialScreens::Message m; m_social.popMessage(m);) {
        messageText(m.color, m.big, m.text.data());
    }
    m_exchange.update(session, m_inventory, nowMs); // phase 8: rolls, mail, auctions, trade
    for (ExchangeScreens::Line l; m_exchange.popLine(l);) {
        if (l.chat) {
            m_social.systemNotice(l.color, l.text.data());
        } else {
            message(l.color, l.big, "%s", l.text.data());
        }
    }
    m_combat.update(session, nowMs);
    m_pets.update(session, nowMs); // phase 15 (pets): the stable's news and reach, the UNIT_COMBATs
    readNotices(session, nowMs); // after the chat and the combat log took this frame's events
    m_pvp.update(session, hud, nowMs); // phase 17 (honor): "HK: <rank>" over the player
    m_popups.update(session, nowMs); // parity (popups): what shows, hides and times them out
    m_battlefield.update(session, nowMs); // phase 17 (queue): the list, the queues' sounds and popups
    m_raidAlerts.update(session, nowMs); // phase 16 (alerts): raid warnings, boss emotes, ReadyCheck's sound
    m_tutorialScreens.update(session, nowMs); // phase 18 (p3): TUTORIAL_TRIGGER, CINEMATIC_STOP
    m_accountFiles.update(session, nowMs);    // phase 18 (p3): the chat windows saved, cache.md5
    m_worldStateScreens.update(session, m_settings, m_travel.worldMapOpen(), nowMs); // phase 17 (scores)
    // Phase 15 (guild, help): the charter's, the tabard designer's and the help desk's lines; what the help windows want.
    m_guild.update(session, nowMs);
    m_help.update(session, nowMs);
    switch (m_help.takeWant()) {
    case HelpScreens::Want::CloseHelp:
        if (m_panels.shown(UiPanel::Help)) {
            togglePanel(session, UiPanel::Help);
        }
        break;
    case HelpScreens::Want::OpenSurvey: // TicketStatusButton with a survey: ShowUIPanel(GMSurveyFrame)
        if (!m_panels.shown(UiPanel::GmSurvey)) {
            togglePanel(session, UiPanel::GmSurvey);
        }
        break;
    case HelpScreens::Want::OpenSuggest:
        if (!m_panels.shown(UiPanel::Suggest)) {
            togglePanel(session, UiPanel::Suggest);
        }
        break;
    case HelpScreens::Want::CloseSurvey:
        if (m_panels.shown(UiPanel::GmSurvey)) {
            togglePanel(session, UiPanel::GmSurvey);
        }
        break;
    case HelpScreens::Want::CloseSuggest:
        if (m_panels.shown(UiPanel::Suggest)) {
            togglePanel(session, UiPanel::Suggest);
        }
        break;
    case HelpScreens::Want::None: break;
    }

    DialogContext c = context(hud, area);
    if (const Entity* me = session.entities().find(session.playerGuid())) {
        c.playerLevel = me->value(uf::UNIT_FIELD_LEVEL);
    }
    const QuestScreens::Frame questInput = questFrame(session, nowMs);
    m_quests.update(questInput); // phase 13 (quests): the log, the watches, the timers

    // Windows of an object close when it is gone or left behind (as the client's frames on their
    // interact-distance checks).
    const auto tooFar = [&](u64 guid) {
        const Entity* me = session.entities().find(session.playerGuid());
        const Entity* other = session.entities().find(guid);
        const bool item = (guid >> 48) == 0x4000; // HIGHGUID_ITEM: a quest from an item has no place
        // Phase 13 (quests): a quest a party member shares stays until answered (the server checks the distance,
        // QUEST_SHARE_DISTANCE, when it is accepted, and tells the sharer).
        const bool sharer = (guid >> 48) == 0 && play.dialog().kind() == game::DialogKind::QuestDetails;
        if (!me || guid == session.playerGuid() || item || sharer ||
            (other && other->type != game::ObjectType::Unit && other->type != game::ObjectType::GameObject &&
             other->type != game::ObjectType::Player)) {
            return false;
        }
        return !other || game::distanceAt(*me, *other, nowMs) - floatValue(*other, uf::UNIT_FIELD_BOUNDINGRADIUS) > 10.0f;
    };
    if (play.dialog().open() && tooFar(play.dialog().npc())) {
        play.closeDialog();
    }
    if (play.loot().open() && tooFar(play.loot().guid())) {
        play.closeLoot();
    }
    if (!play.loot().open()) {
        m_lootGuid = 0; // the same corpse opened again is LOOT_OPENED again
    }

    // Parity pass (panels): what the modules opened and closed since last frame goes through the original's panel
    // rules (UIPanelWindows); then where the windows go this frame, clear of the stick and the action buttons.
    m_microLayout = microMenuLayout(c);
    const UiPanelPlaces places = panelPlaces(hud, c);
    m_panels.setCapacity(places.right.w > 0.0f ? 2 : 1);
    if (m_inventory.updateContainers(session, nowMs)) { // phase 15 (items): BAG_OPEN, BAG_CLOSED
        syncBags(session);
    }
    m_dressUp.update(session); // phase 15 (items): the auction house's dressing room closes with it
    syncPanels(session);
    m_battlefield.panelShown(m_panels.shown(UiPanel::Battlefield)); // phase 17 (queue): OnShow / OnHide
    followTargeting(session); // phase 13 (spells): the item choice's windows; a window over the place drops it
    const bool panelsUp = m_panels.any();

    // Not modal: the quest timers under the minimap (not under a window on the right), the chat, the messages.
    ui::Gui g{c.ui, c.draw};
    if (m_quests.timersShown() && !m_rightPlaceUsed) {
        // Phase 13 (quests): QuestTimerFrame under the minimap cluster (UIParent_ManageFramePositions); a tap opens the
        // log on that quest (the timer's OnClick).
        const f32 top = area.y + g.unit(200.0f) + m_worldStateScreens.captureBarsHeight(c); // phase 17: the bars
        if (m_quests.drawTimers(questInput, c, area.right() - g.px(6.0f), top) != 0 &&
            !m_panels.shown(UiPanel::QuestLog)) {
            togglePanel(session, UiPanel::QuestLog);
        }
    }
    m_combatText.draw(c, m_settings, nowMs);  // phase 15 (options): under every frame, as the addon's
    m_combat.drawHud(session, hud, c, nowMs); // pet frame and bar, combo points, the target's cast bar
    // Phase 15 (character): ReputationWatchBar on the experience bar; the character and inspect windows' state.
    m_character.update(session, m_panels.shown(UiPanel::Character), m_unitMenus.inspectOpen(session));
    m_character.drawWatchBar(session, c, hud.experienceBarRect(), nowMs);
    m_unitMenus.drawFollowStatus(c, nowMs);   // phase 13 (social): AutoFollowStatus over the world
    m_auras.drawPetAuras(session, c, m_combat.petFrameRect(), nowMs); // parity: the pet frame's buffs
    m_pets.drawHud(session, hud, c, m_combat.petFrameRect(), nowMs); // phase 15 (pets): Group N, hit indicators
    // Party frames under the player frame (and its pet's); the chat at the bottom left, clear of them and left of
    // the action buttons (parity: ChatFrame1 with the Combat Log docked as a tab). A panel over the chat hides it
    // (the original's windows cover ChatFrame1); open for typing, it goes over the panels instead.
    // Phase 20 (assists): a healer's template brings them beside the touch cluster.
    m_social.setPartyDock(m_touch.healerDock(hud.actionBarRect(), c.safeArea, places.stickPad), m_touch.mirrored());
    m_social.partyFrames(session, c, std::max(area.y + g.unit(100.0f), m_combat.petFrameBottom()));
    m_raidPullouts.keepClearOfHud(hud, places.stickPad, m_microLayout.strip, c); // phase 16 (pullouts)
    m_raidPullouts.draw(session, c, nowMs); // phase 16 (raids): the pullout frames, under the panels
    // Phase 17 (scores): WorldStateAlwaysUpFrame and the capture bars, BattlefieldMinimap (under the panels).
    m_worldStateScreens.drawHud(c, hud, nowMs);
    m_worldStateScreens.drawBattlefieldMinimap(session, c, hud, m_settings, nowMs);
    const bool chatOverPanels = m_social.chatExpanded();
    m_questWatchRect = {};
    if (!chatOverPanels && (!panelsUp || !uiPanelOverlaps(m_social.chatRect(), places.both))) {
        m_social.chat(session, c, input, hud.actionBarRect(), nowMs);
        // QuestWatchFrame over the chat; a quick tap on a quest opens the log on it.
        const ui::Rect watch = questWatchPlace(c, area);
        if (m_quests.watchShown() && watch.w > 0.0f &&
            m_quests.drawWatch(questInput, c, watch.x, watch.y, watch.bottom(), watch.w, m_questWatchRect) != 0 &&
            !m_panels.shown(UiPanel::QuestLog)) {
            togglePanel(session, UiPanel::QuestLog);
        }
    }
    m_inventory.drawHud(session, c, nowMs); // the rim's backpack (phase 20: always, the bag bar unfolds from it)
    if (m_inventory.takeBackpackTap()) {
        toggleBags(session);
    }
    if (m_inventory.takeBackpackHold()) { // phase 20 (retouches): the bag bar the gear on the rim used to unfold
        m_microState.bagsUnfolded = !m_microState.bagsUnfolded;
    }
    if (m_inventory.takeDurabilityTap() && !m_panels.shown(UiPanel::Character)) {
        togglePanel(session, UiPanel::Character); // see what needs repairing
    }
    // Parity pass: the minimap cluster (its letter replaces ExchangeScreens::drawHud's: MiniMapMailFrame).
    m_minimap.drawGame(session, hud.output().minimap, c, m_settings, nowMs);
    m_battlefield.drawMinimapButton(session, c, m_minimap.shown(), nowMs); // phase 17 (queue)
    m_travel.drawOverlay(session, c, nowMs, [](void* self, u32 color, bool big, const char* line) {
        static_cast<GameScreens*>(self)->messageText(color, big, line);
    }, this);
    m_units.drawOverlay(session, c, hud, nowMs); // parity: the unit, PvP badge and rest state tooltips
    m_objectTooltip.draw(session, c, hud);       // phase 16 (alerts)
    {
        // Phase 15 (help): TicketStatusFrame at the top right, left of the minimap cluster (TOPRIGHT -180, 0).
        const MinimapLayout map = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
        const ui::Rect left{area.x, area.y, map.left - area.x - g.px(4.0f), area.h};
        m_help.drawStatus(session, c, left, m_panels.shown(UiPanel::Help), nowMs);
    }
    m_tutorialScreens.drawAlerts(&session, c, hudBottom(hud, c), hud.actionBarRect(), nowMs); // phase 18 (p3)

    // The panels: the original's left / center slots, the bags beside them (parity pass: not modal, the stick,
    // the camera and the action buttons keep working).
    m_targeting.drawWorld(session, c, hud.output()); // phase 13 (spells): the circle, under the windows
    drawPanels(session, c, input, places, nowMs);
    // The micro menu's gear on the minimap's rim; phase 20 (retouches): what it unfolds goes over the windows (a full
    // screen one, the world map, covers it).
    microMenu(session, c);
    m_inventory.dropOnBagBar(session, c, m_bagRects); // phase 15 (items): items dropped on the bag bar
    // Phase 15 (options): /macro opens the macro window; a macro picked up there goes to the next action button
    // tapped; the extra bars the HUD drew take their pages out of the main bar's.
    if (m_social.takeMacroWanted() && !m_panels.shown(UiPanel::Macro)) {
        togglePanel(session, UiPanel::Macro);
    }
    if (const game::ActionButton picked = m_macros.takePickup(); !picked.empty()) {
        m_combat.placeOnBar(picked);
    }
    m_combat.setDrawnMultiBars(hud.drawnMultiBars());
    if (chatOverPanels) {
        m_social.chat(session, c, input, hud.actionBarRect(), nowMs);
    }
    m_tutorialScreens.drawFrame(&session, c, hudBottom(hud, c), hud.actionBarRect()); // phase 18 (p3): HIGH strata

    // Over the panels, as the original's higher frames: the breath, fatigue and feign death bars, the UIErrorsFrame
    // under them.
    mirrorTimerViews(play.mirrorTimers(), m_data, nowMs, m_mirrorBars);
    const f32 barsBottom = mirrorTimerBars(c, m_mirrorBars, m_social.chatRect()); // clear of the chat's lines
    // Phase 16 (alerts): ReadyCheckFrame (MEDIUM strata: under the messages), clear of the action buttons.
    m_raidAlerts.drawReadyCheck(session, c, area, barsBottom, hud.actionBarRect(), nowMs);
    drawMessages(hud, area, nowMs, barsBottom, m_social.chatRect());
    // Phase 16 (alerts): RaidWarningFrame and RaidBossEmoteFrame under the UIErrorsFrame.
    m_raidAlerts.drawFrames(c, area, barsBottom, m_social.chatRect(), nowMs);

    // Phase 20 (edit): the touch cluster's windows (the template list, the edit mode's panel, the drawer).
    m_touch.edit().setFrame(&input, hud.actionBarRect());
    m_touch.draw(session, m_combat, c, nowMs);

    // Modal steps, one at a time over everything: the action editor, a player's sheet, menus... The questions
    // (StaticPopups: death, invitations, logout...) are not modal: app/PopupScreens draws them last.
    deathHint(session, c, nowMs); // the ghost's hint (not modal)
    if (m_combat.hasWindow(session)) {
        m_combat.drawWindow(session, c, nowMs); // the action editor and its pickers, the trainer's questions
    } else if (m_social.wantsPopup()) {
        m_social.drawModal(session, c, input, nowMs); // invitations, a player's sheet, menus, questions
    } else if (m_travel.wantsModal(session)) { // phase 8: the innkeeper's question
        m_travel.drawModal(session, c, nowMs);
    } else if (m_unitMenus.wantsModal()) { // phase 13 (social): a unit's menu (UnitPopup)
        m_unitMenus.drawModal(session, c);
    }
    m_exchange.drawOverlay(session, c, m_inventory, nowMs); // phase 8: loot rolls, the master looter's picker
    // A long press on an item of the loot or quest windows: its tooltip (the windows only report it).
    u32 pressedItem = 0;
    ui::Rect pressedRect;
    if (kit::takeLongPressedItem(pressedItem, pressedRect)) {
        m_inventory.showItemTooltip(pressedItem, pressedRect);
    }
    m_raidPullouts.drawOverlay(session, c, nowMs); // phase 16 (pullouts): a pullout's menu, items dropped on members
    m_battlefield.drawOverlay(session, c, nowMs); // phase 17 (queue): its tooltips, the minimap menu
    m_auras.drawOverlay(session, c, nowMs); // parity: an aura's tooltip, a party member's aura list
    m_inventory.drawOverlay(session, c, nowMs); // item tooltips, item dialogs, the item being carried
    m_targeting.drawBar(session, c, hud.actionBarRect()); // phase 13 (spells): Cast / Cancel, under popups
    m_popups.draw(session, c, input, nowMs);    // parity (popups): the StaticPopups, over everything but...
    drawFullScreenPanel(session, c, input, nowMs); // ...the option windows (FULLSCREEN strata)
    windowSounds(); // phase 9
}

void GameScreens::windowSounds() {
    // The windows the player opens, with the sounds of the original frames (FrameXML's PlaySound in their OnShow /
    // OnHide). What the server opens (NPCs, loot, vendor, bank, trade, auction house, mailbox, taxi) GameAudio follows
    // in the game state itself.
    struct WindowSound {
        UiSound open;
        UiSound close;
    };
    static constexpr WindowSound kSounds[] = {
        {UiSound::GameMenuOpen, UiSound::GameMenuClose},   // game menu
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // settings (OptionsFrame)
        {UiSound::QuestLogOpen, UiSound::QuestLogClose},   // quest log
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // a question (StaticPopup)
        {UiSound::BagOpen, UiSound::BagClose},             // bags
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // character sheet, skills, reputation
        {UiSound::SpellBookOpen, UiSound::SpellBookClose}, // spell book
        {UiSound::TalentsOpen, UiSound::TalentsClose},     // talents
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // social window (FriendsFrame)
        {UiSound::QuestLogOpen, UiSound::QuestLogClose},   // world map
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // phase 13: TradeSkillFrame (igCharacterInfoOpen)
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // phase 13: CraftFrame
        {UiSound::NpcOpen, UiSound::NpcClose},             // phase 15: GuildRegistrarFrame (igQuestListOpen)
        {UiSound::NpcOpen, UiSound::NpcClose},             // phase 15: PetitionFrame
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // phase 15: HelpFrame (igCharacterInfoOpen)
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // phase 15: SuggestFrame (igMainMenuOpen)
    };
    const bool shown[] = {
        m_panels.shown(UiPanel::GameMenu),
        m_panels.shown(UiPanel::Options),
        m_panels.shown(UiPanel::QuestLog),
        false, // parity (popups): app/PopupScreens plays each popup's own (StaticPopup_OnShow / OnHide)
        false, // phase 15 (items): each ContainerFrame plays its own (InventoryScreens::updateContainers)
        m_inventory.isOpen(InventoryScreens::Window::Character), // with its Skills and Reputation tabs
        m_combat.isOpen(CombatWindow::Spellbook),
        m_combat.isOpen(CombatWindow::Talents),
        m_social.windowWanted(),
        m_travel.worldMapOpen(),
        m_panels.shown(UiPanel::TradeSkill), // phase 13
        m_panels.shown(UiPanel::Craft),
        m_panels.shown(UiPanel::GuildRegistrar), // phase 15
        m_panels.shown(UiPanel::Petition),
        m_panels.shown(UiPanel::Help),
        m_panels.shown(UiPanel::Suggest),
    };
    static_assert(std::size(kSounds) == std::size(shown), "one sound pair per window");
    u32 windows = 0;
    for (u32 i = 0; i < std::size(shown); ++i) {
        windows |= shown[i] ? 1u << i : 0u;
    }
    const u32 opened = windows & ~m_soundWindows;
    const u32 closed = m_soundWindows & ~windows;
    for (u32 i = 0; i < std::size(shown); ++i) {
        if (closed & (1u << i)) {
            // A game menu button: the original plays its option sound, not the menu closing.
            m_uiSound(i == 0 && opened != 0 ? UiSound::GameMenuOption : kSounds[i].close);
        }
        if (opened & (1u << i)) {
            m_uiSound(kSounds[i].open);
        }
    }
    m_soundWindows = windows;
    // Parity pass: the settings' check boxes (OptionsFrame: igMainMenuOptionCheckBoxOn / Off).
    // Phase 15 (options): the video window's; the interface and sound windows play their own (app/OptionsScreens).
    const u8 toggles = static_cast<u8>((m_settings.limitFps ? 1 : 0) | (m_settings.showFps ? 2 : 0) |
                                       (m_settings.dynamicQuality ? 4 : 0) | (m_settings.fullScreenGlow ? 8 : 0) |
                                       (m_settings.deathEffect ? 16 : 0) | (m_settings.terrainHighlights ? 32 : 0));
    if (m_soundToggles != 0xFF && toggles != m_soundToggles) {
        m_uiSound((toggles & ~m_soundToggles) != 0 ? UiSound::CheckBoxOn : UiSound::CheckBoxOff);
    }
    m_soundToggles = toggles;
}

} // namespace vd
