#include "app/UnitMenuScreens.h"

#include "app/CharacterScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/InventoryScreens.h"
#include "app/PlayerController.h"
#include "app/PopupScreens.h"
#include "app/SocialScreens.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/ItemData.h"
#include "game/Reactions.h"
#include "game/Social.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using B = game::UnitPopupButton;
using game::Entity;
using game::UnitPopupMenu;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26); // UI_ERROR_MESSAGE (1, 0.1, 0.1)
constexpr u32 kUnitFlagPetRename = 0x10;       // UNIT_FLAG_PET_RENAME (Objects/UnitDefines.h)
constexpr u32 kUnitFlagPetAbandon = 0x20;      // UNIT_FLAG_PET_ABANDON
constexpr u32 kUnitFlagStunned = 0x40000;      // UNIT_FLAG_STUNNED: FollowUnit refuses (WoW.exe 0x60ff95)
constexpr f32 kFollowNear = 3.0f;    // yards: the follower stops this close...
constexpr f32 kFollowResume = 4.5f;  // ...and runs again past this
constexpr u32 kFollowFadeMs = 4000;  // AUTOFOLLOW_STATUS_FADETIME
constexpr u32 kVisibleItemStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;

// ToggleDropDownMenu's anchors: TOPLEFT of the list at the frame's BOTTOMLEFT plus these units (y up).
struct Anchor {
    f32 x = 0.0f;
    f32 y = 0.0f;
};
constexpr Anchor anchorOf(UnitMenuScreens::Frame from) {
    switch (from) {
    case UnitMenuScreens::Frame::Player: return {106.0f, 27.0f}; // PlayerFrame_OnClick
    case UnitMenuScreens::Frame::Target: return {120.0f, 10.0f}; // TargetFrame_OnClick
    case UnitMenuScreens::Frame::Party: return {47.0f, 15.0f};   // PartyMemberFrame_OnClick
    case UnitMenuScreens::Frame::Pet: return {8.0f, 22.0f};      // PetFrameDropDown's own place
    default: return {};
    }
}

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view name, std::string_view prefix) {
    return prefix.size() <= name.size() && sameName(name.substr(0, prefix.size()), prefix);
}

std::string_view copyName(std::array<char, 64>& out, std::string_view s) {
    const usize n = std::min(s.size(), out.size() - 1);
    std::memcpy(out.data(), s.data(), n);
    out[n] = 0;
    return {out.data(), n};
}

f32 distance3(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

bool isPlayer(const Entity& e) {
    return e.type == game::ObjectType::Player;
}

// UnitCanCooperate("player", unit): two players of the same side.
bool canCooperate(const Entity& me, const Entity& other, const game::Reactions* reactions) {
    return isPlayer(me) && isPlayer(other) &&
           (other.guid == me.guid || game::reactionTo(me, other, reactions) == game::Reaction::Friendly);
}

} // namespace

void UnitMenuScreens::init(const game::GameData* data) {
    m_data = data;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
}

void UnitMenuScreens::reset() {
    m_menu = OpenMenu{};
    m_follow = Follow{};
    m_inspect = 0;
    m_openCharacter = false;
}

void UnitMenuScreens::message(u32 color, std::string_view line) {
    if (m_message && !line.empty()) {
        char buffer[256];
        const usize n = std::min(line.size(), sizeof(buffer) - 1);
        std::memcpy(buffer, line.data(), n);
        buffer[n] = 0;
        m_message(m_messageContext, color, false, buffer);
    }
}

void UnitMenuScreens::error(std::string_view key) {
    message(kErrorColor, text::get(text::Key(key)));
}

std::string_view UnitMenuScreens::nameOf(GameSession& session, u64 guid) const {
    const Entity* e = guid ? session.entities().find(guid) : nullptr;
    if (!e) {
        const std::string* known = guid ? session.playerName(guid) : nullptr;
        return known ? std::string_view(*known) : std::string_view{};
    }
    if (isPlayer(*e)) {
        const std::string* known = session.playerName(guid);
        return known ? std::string_view(*known) : std::string_view{};
    }
    if (game::isUnit(*e)) {
        if (const net::CreatureQueryInfo* creature = session.creature(e->value(uf::OBJECT_FIELD_ENTRY))) {
            return creature->name;
        }
    }
    return {};
}

// --- Opening -------------------------------------------------------------------------------------------------

void UnitMenuScreens::open(GameSession& session, UnitPopupMenu menu, u64 guid, std::string_view name, f32 x,
                           f32 y, bool guildTab) {
    m_menu = OpenMenu{};
    m_menu.menu = menu;
    m_menu.guildTab = guildTab;
    m_menu.guid = guid;
    copyName(m_menu.name, name);
    m_menu.x = x;
    m_menu.y = y;
    // UnitPopup_ShowMenu: only CANCEL to show (a creature without the right to mark it, a stranger's pet...): no
    // menu at all.
    const game::UnitPopupState s = state(session);
    m_menu.open = game::unitPopupItems(menu, s, m_level1) > 0;
    if (m_trace) {
        VD_LOG_INFO("Unit menu %u for %llx (%s): %s", static_cast<u32>(menu), static_cast<unsigned long long>(guid),
                    m_menu.name.data(), m_menu.open ? "open" : "nothing to show");
    }
}

void UnitMenuScreens::openUnitMenu(GameSession& session, u64 guid, Frame from, const ui::Rect& frame) {
    if (!session.inWorld() || guid == 0) {
        return;
    }
    const game::Gameplay& play = session.play();
    UnitPopupMenu menu = UnitPopupMenu::Self;
    switch (from) {
    case Frame::Player: menu = UnitPopupMenu::Self; break;
    case Frame::Party: menu = UnitPopupMenu::Party; break;
    case Frame::Pet: menu = UnitPopupMenu::Pet; break;
    case Frame::Cursor:
        if (play.social().inGroupWith(guid)) {
            menu = UnitPopupMenu::Party; // the group tab's party members (the raid's have their own menu)
            break;
        }
        [[fallthrough]];
    default: {
        // TargetFrameDropDown_Initialize. UnitInParty: the player's party (its subgroup in a raid).
        const Entity* e = session.entities().find(guid);
        const game::GroupState& group = play.social().group();
        bool inParty = false;
        for (const game::GroupMember& m : group.members) {
            inParty = inParty || (m.guid == guid && (!group.raid || m.subgroup == group.ownSubgroup));
        }
        menu = game::targetFrameMenu(guid == session.playerGuid(), guid == play.abilities().pet().guid,
                                     e && isPlayer(*e), inParty);
        break;
    }
    }
    const Anchor a = anchorOf(from);
    // Units of the 768-high interface: the frames' own scale (app/UnitFrames draws them in those units).
    // The frame's own units: PlayerFrame and TargetFrame 232 wide, PetFrame 128, a party frame as
    // app/SocialWindows draws it (120).
    const f32 unit = frame.w > 0.0f ? frame.w / (from == Frame::Party ? 120.0f : from == Frame::Pet ? 128.0f
                                                                                                   : 232.0f)
                                    : 1.0f;
    open(session, menu, guid, menu == UnitPopupMenu::RaidTargetIcon ? text::get("RAID_TARGET_ICON")
                                                                    : nameOf(session, guid),
         frame.x + a.x * unit, frame.bottom() - a.y * unit);
    m_menu.unitIsTarget = from == Frame::Target;
}

void UnitMenuScreens::openFriendMenu(GameSession& session, std::string_view name, f32 x, f32 y, bool guildTab) {
    if (!session.inWorld() || name.empty()) {
        return;
    }
    open(session, UnitPopupMenu::Friend, 0, name, x, y, guildTab);
}

void UnitMenuScreens::openRaidMenu(GameSession& session, u64 guid, std::string_view name, f32 x, f32 y) {
    if (!session.inWorld()) {
        return;
    }
    open(session, UnitPopupMenu::Raid, guid, name, x, y);
}

void UnitMenuScreens::actOnHud(GameSession& session, const Hud& hud, bool petFrameHeld, const ui::Rect& petFrame) {
    if (!session.inWorld()) {
        return;
    }
    const HudOutput& out = hud.output();
    if (out.playerFrameHold) {
        openUnitMenu(session, session.playerGuid(), Frame::Player, hud.playerFrameRect());
    }
    if (out.targetFrameHold && session.target() != 0) {
        openUnitMenu(session, session.target(), Frame::Target, hud.targetFrameRect());
    }
    if (petFrameHeld && session.play().abilities().pet().active()) {
        openUnitMenu(session, session.play().abilities().pet().guid, Frame::Pet, petFrame);
    }
}

// --- The rules' inputs -----------------------------------------------------------------------------------------

game::UnitPopupState UnitMenuScreens::state(GameSession& session) const {
    game::UnitPopupState s;
    game::Gameplay& play = session.play();
    const game::Social& social = play.social();
    const game::GroupState& group = social.group();
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    const game::Reactions* reactions = m_reactions;
    const u32 now = session.lastUpdateMs();

    s.inParty = group.active;
    s.leader = social.isLeader();
    s.assistant = group.active && group.raid && group.ownAssistant;
    s.playerDead = me && game::isDead(*me) && !play.death().ghost;
    s.playerDeadOrGhost = me && (game::isDead(*me) || play.death().ghost);
    s.fullControl = play.control().playerHasControl(); // HasFullControl: not feared, charmed or possessed
    s.canShowResetInstances = play.travel().canShowResetInstances(now);
    s.guildLeader = social.guild().id != 0 && social.guild().rank == 0;
    s.guildFrameVisible = m_menu.guildTab;
    s.lootMethod = group.lootMethod;
    s.lootThreshold = group.lootThreshold;
    const game::PetState& pet = play.abilities().pet();
    if (const Entity* p = pet.active() ? entities.find(pet.guid) : nullptr) {
        const u32 flags = p->value(uf::UNIT_FIELD_FLAGS);
        s.petCanBeAbandoned = (flags & kUnitFlagPetAbandon) != 0;
        s.petCanBeRenamed = (flags & kUnitFlagPetRename) != 0;
    }

    const u64 guid = m_menu.guid;
    const Entity* unit = guid ? entities.find(guid) : nullptr;
    s.hasUnit = m_menu.menu != UnitPopupMenu::Friend && guid != 0;
    if (s.hasUnit) {
        // A member out of sight is still a "partyN" unit of the player's side: its group status says whether it
        // is online and dead.
        s.canCooperate = me && (unit ? canCooperate(*me, *unit, reactions) : social.inGroupWith(guid));
        s.unitDeadOrGhost = unit && game::isDead(*unit);
        s.unitConnected = unit != nullptr || guid == session.playerGuid();
        for (const game::GroupMember& m : group.members) {
            if (m.guid == guid) {
                s.unitConnected = (m.status & net::kMemberOnline) != 0;
                s.unitDeadOrGhost = unit ? s.unitDeadOrGhost : (m.status & (net::kMemberDead | net::kMemberGhost)) != 0;
            }
        }
        s.unitIsMasterLooter = group.lootMethod == static_cast<u8>(net::LootMethod::MasterLoot) &&
                               (group.looter == guid || (group.looter == 0 && guid == session.playerGuid()));
        for (u32 i = 1; i < s.inDistance.size(); ++i) {
            s.inDistance[i] = me && unit && distance3(me->positionAt(now), unit->positionAt(now)) <=
                                                game::kInteractDistances[i];
        }
        s.unitRaidTarget = play.commands().raidTarget(guid);
    }
    const u64 target = play.target();
    const Entity* t = target ? entities.find(target) : nullptr;
    s.targetExists = t != nullptr;
    if (t) {
        const u64 owner = game::isUnit(*t) ? game::guidValue(*t, uf::UNIT_FIELD_SUMMONEDBY) : 0;
        s.targetInGroup = social.inGroupWith(target) || target == session.playerGuid() ||
                          (owner != 0 && (social.inGroupWith(owner) || owner == session.playerGuid()));
        s.targetIsPlayer = isPlayer(*t);
        s.targetCanCooperate = me && canCooperate(*me, *t, reactions);
        s.targetIsSelf = target == session.playerGuid();
        s.targetRaidTarget = play.commands().raidTarget(target);
    }
    const std::string_view name = m_menu.name.data();
    s.hasName = !name.empty();
    const std::string* own = session.playerName(session.playerGuid());
    s.nameIsSelf = own && sameName(*own, name);
    for (const game::GroupMember& m : group.members) {
        s.nameInParty = s.nameInParty || ((!group.raid || m.subgroup == group.ownSubgroup) && sameName(m.name, name));
    }
    s.raidRank = 0;
    if (m_menu.menu == UnitPopupMenu::Raid) {
        if (guid == group.leader) {
            s.raidRank = 2;
        } else if (guid == session.playerGuid()) {
            s.raidRank = group.ownAssistant ? 1 : 0;
        } else {
            for (const game::GroupMember& m : group.members) {
                if (m.guid == guid) {
                    s.raidRank = m.assistant ? 1 : 0;
                }
            }
        }
    }
    return s;
}

// --- Drawing -----------------------------------------------------------------------------------------------------

std::string_view UnitMenuScreens::itemText(B button, const game::UnitPopupState& s, std::span<char> buffer) const {
    (void)buffer;
    switch (button) {
    case B::LootMethod: // UnitPopup_ShowMenu: the button reads as the method in use
        if (const char* key = game::lootMethodKey(s.lootMethod)) {
            return text::get(key);
        }
        break;
    case B::LootThreshold:
        if (const char* key = game::itemQualityKey(s.lootThreshold)) {
            return text::get(key);
        }
        break;
    default: break;
    }
    return text::get(text::Key(game::unitPopupButtonInfo(button).text));
}

void UnitMenuScreens::fillItems(std::span<const game::UnitPopupItem> items, const game::UnitPopupState& s,
                                bool level2) {
    for (u32 i = 0; i < items.size(); ++i) {
        const game::UnitPopupItem& item = items[i];
        const game::UnitPopupButtonInfo& info = game::unitPopupButtonInfo(item.button);
        DropDownItemView& v = level2 ? m_subViews[i] : m_views[i + 1];
        std::array<char, 64>& buffer = level2 ? m_subTexts[i] : m_texts[i + 1];
        v = DropDownItemView{};
        v.text = copyName(buffer, itemText(item.button, s, buffer));
        v.color = info.color;
        if (item.button == B::LootThreshold) { // in its quality's colour
            v.color = ui::qualityColor(s.lootThreshold);
        }
        v.enabled = item.enabled;
        v.checkable = item.checkable;
        v.checked = item.checked;
        v.hasArrow = item.nested;
        v.raidIcon = info.icon;
    }
}

void UnitMenuScreens::drawModal(GameSession& session, DialogContext& c) {
    VD_PROFILE_ZONE("UnitMenuScreens::modal");
    if (!m_menu.open || !session.inWorld()) {
        m_menu.open = false;
        return;
    }
    // The unit went away (out of sight, the target changed for the target's menu): the menu goes with it, as
    // TargetFrame_OnHide's CloseDropDownMenus.
    if (m_menu.guid != 0 && m_menu.menu != UnitPopupMenu::Raid && !session.entities().find(m_menu.guid) &&
        m_menu.menu != UnitPopupMenu::Party) {
        m_menu.open = false;
        return;
    }
    const game::UnitPopupState s = state(session);
    m_level1Count = game::unitPopupItems(m_menu.menu, s, m_level1);
    if (m_level1Count == 0) {
        m_menu.open = false;
        return;
    }
    // The title (the unit's name, or UNKNOWN), then the buttons.
    const std::string_view name = m_menu.name.data();
    m_views[0] = DropDownItemView{};
    m_views[0].text = copyName(m_texts[0], name.empty() ? text::get("UNKNOWN") : name);
    m_views[0].title = true;
    fillItems({m_level1.data(), m_level1Count}, s, false);
    m_level2Count = 0;
    i32 nestedRow = -1;
    if (m_menu.nestedRow >= 0) {
        // Still there and still nested (the leader may have changed): its second level.
        for (u32 i = 0; i < m_level1Count; ++i) {
            if (m_level1[i].button == m_menu.nested && m_level1[i].nested) {
                nestedRow = static_cast<i32>(i);
            }
        }
        if (nestedRow >= 0) {
            m_level2Count = game::unitPopupSubItems(m_menu.nested, s, m_level2);
            fillItems({m_level2.data(), m_level2Count}, s, true);
        }
    }
    DropDownMenuView view;
    view.x = m_menu.x;
    view.y = m_menu.y;
    view.level1 = {m_views.data(), m_level1Count + 1};
    view.level2 = {m_subViews.data(), m_level2Count};
    view.level2Row = nestedRow >= 0 ? nestedRow + 1 : -1;
    const DropDownMenuResult r = dropDownMenu(c, "unitMenu", view);
    if (r.toggled > 0) {
        const game::UnitPopupItem& item = m_level1[static_cast<u32>(r.toggled - 1)];
        const bool same = m_menu.nestedRow >= 0 && m_menu.nested == item.button;
        m_menu.nestedRow = same ? -1 : r.toggled - 1;
        m_menu.nested = item.button;
        return;
    }
    if (r.picked >= 0) {
        const B button = r.level == 1 ? m_level1[static_cast<u32>(r.picked - 1)].button
                                      : m_level2[static_cast<u32>(r.picked)].button;
        if (m_trace) {
            VD_LOG_INFO("Unit menu: %s", game::unitPopupButtonInfo(button).text);
        }
        act(session, button); // UnitPopup_OnClick; UIDropDownMenuButton_OnClick closes the menus
        m_menu.open = false;
        return;
    }
    if (r.closed) {
        m_menu.open = false;
    }
}

// --- UnitPopup_OnClick -------------------------------------------------------------------------------------------

void UnitMenuScreens::act(GameSession& session, B button) {
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    const game::GroupState& group = social.group();
    const u64 guid = m_menu.guid;
    const std::string_view name = m_menu.name.data();
    switch (button) {
    case B::Trade: initiateTrade(session, guid); break;
    case B::Whisper:
        if (m_social) {
            m_social->sendTell(name); // ChatFrame_SendTell
        }
        break;
    case B::Inspect: inspectUnit(session, guid, m_menu.unitIsTarget); break;
    case B::Target: targetByName(session, name, true); break;
    case B::Duel: // StartDuelUnit
        if (play.castSpell(game::kDuelSpell, guid)) {
            social.challengeSent(guid);
        }
        break;
    case B::Invite: social.inviteToGroup(name); break; // InviteToParty(unit) / InviteByName(name)
    case B::Uninvite: social.uninvite(guid); break;
    case B::Promote: social.promoteToLeader(guid); break;
    case B::GuildPromote:
        if (m_popups) {
            m_popups->askGuildPromote(session, name); // CONFIRM_GUILD_PROMOTE
        }
        break;
    case B::GuildLeave:
        if (m_popups) {
            m_popups->askGuildLeave(session); // CONFIRM_GUILD_LEAVE
        }
        break;
    case B::Leave: social.leaveGroup(); break;
    case B::Follow: followUnit(session, guid); break; // FollowByName(name, 1): the unit itself here
    case B::PetDismiss: play.abilities().dismissPet(); break;
    case B::PetAbandon:
        if (m_popups) {
            m_popups->askAbandonPet(session);
        }
        break;
    case B::PetPaperdoll: m_openCharacter = true; break;
    case B::PetRename:
        if (m_popups) {
            m_popups->askRenamePet(session); // RENAME_PET, then PETRENAMECONFIRM
        }
        break;
    case B::FreeForAll:
    case B::RoundRobin:
    case B::MasterLooter:
    case B::GroupLoot:
    case B::NeedBeforeGreed: {
        // SetLootMethod: "master" names the menu's unit (the player in its own menu).
        const auto method = static_cast<net::LootMethod>(static_cast<u8>(button) - static_cast<u8>(B::FreeForAll));
        social.setLootMethod(method, guid != 0 ? guid : session.playerGuid(), group.lootThreshold);
        break;
    }
    case B::LootPromote:
        social.setLootMethod(net::LootMethod::MasterLoot, guid != 0 ? guid : session.playerGuid(),
                             group.lootThreshold);
        break;
    case B::ResetInstances:
        if (m_popups) {
            m_popups->askResetInstances(session);
        }
        break;
    case B::Quality2:
    case B::Quality3:
    case B::Quality4:
        social.setLootMethod(static_cast<net::LootMethod>(group.lootMethod),
                             group.looter ? group.looter : session.playerGuid(),
                             static_cast<u32>(button) - static_cast<u32>(B::Quality2) + 2);
        break;
    case B::RaidLeader: social.promoteToLeader(guid); break;  // PromoteByName
    case B::RaidPromote: play.commands().setAssistant(guid, true); break;
    case B::RaidDemote: play.commands().setAssistant(guid, false); break;
    case B::RaidRemove: social.uninvite(guid); break; // UninviteFromRaid
    default:
        if (game::isRaidTargetButton(button)) {
            // SetRaidTargetIcon(unit, index): the icon it wears again takes it off.
            const u8 current = play.commands().raidTarget(guid);
            play.commands().setRaidTarget(guid, game::toggledRaidTarget(current, game::raidTargetOfButton(button)));
        }
        break;
    }
}

// --- The C functions -------------------------------------------------------------------------------------------

u64 UnitMenuScreens::findByName(GameSession& session, std::string_view name, bool exact) const {
    if (name.empty()) {
        return 0;
    }
    const Entity* me = session.entities().find(session.playerGuid());
    const u32 now = session.lastUpdateMs();
    const glm::vec3 from = me ? me->positionAt(now) : glm::vec3(0.0f);
    u64 best = 0;
    f32 bestDistance = 0.0f;
    session.entities().forEach([&](const Entity& e) {
        if (!game::isUnit(e)) {
            return;
        }
        const std::string_view its = nameOf(session, e.guid);
        if (its.empty() || !(exact ? sameName(its, name) : startsWith(its, name))) {
            return;
        }
        const f32 d = distance3(from, e.positionAt(now));
        if (best == 0 || d < bestDistance) {
            best = e.guid;
            bestDistance = d;
        }
    });
    return best;
}

bool UnitMenuScreens::targetByName(GameSession& session, std::string_view name, bool exact) {
    const u64 guid = findByName(session, name, exact);
    if (guid == 0) {
        error(name.empty() ? "ERR_GENERIC_NO_TARGET" : "ERR_UNIT_NOT_FOUND"); // WoW.exe 0x489d60
        return false;
    }
    session.setTarget(guid);
    return true;
}

bool UnitMenuScreens::assistUnit(GameSession& session, u64 guid) {
    const Entity* e = guid ? session.entities().find(guid) : nullptr;
    if (!e || !game::isUnit(*e)) {
        error("ERR_GENERIC_NO_TARGET");
        return false;
    }
    const u64 its = game::guidValue(*e, uf::UNIT_FIELD_TARGET);
    if (its == 0 || !session.entities().find(its)) {
        return false; // nothing to assist with: the target stays
    }
    session.setTarget(its);
    // Phase 15 (options): ASSIST_ATTACK ("assistAttack"): an assisted target that can be attacked is attacked.
    const Entity* me = session.entities().find(session.playerGuid());
    const Entity* target = session.entities().find(its);
    if (m_assistAttack && me && target && game::isUnit(*target) && !game::isDead(*target) &&
        game::canAttack(*me, *target, m_reactions)) {
        session.play().startAttack(its);
    }
    return true;
}

bool UnitMenuScreens::assistByName(GameSession& session, std::string_view name) {
    const u64 guid = findByName(session, name, false);
    if (guid == 0) {
        error("ERR_UNIT_NOT_FOUND");
        return false;
    }
    return assistUnit(session, guid);
}

bool UnitMenuScreens::followByName(GameSession& session, std::string_view name, bool exact) {
    const u64 guid = findByName(session, name, exact);
    if (guid == 0) {
        error(name.empty() ? "ERR_GENERIC_NO_TARGET" : "ERR_UNIT_NOT_FOUND");
        return false;
    }
    return followUnit(session, guid);
}

bool UnitMenuScreens::followUnit(GameSession& session, u64 guid) {
    // WoW.exe 1.12.1 0x60fed0 (mode 3) and 0x6110a0: a player of the player's side, the player alive, not
    // stunned nor busy (a taxi), within 30 yards.
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    const Entity* leader = guid ? entities.find(guid) : nullptr;
    if (!me || guid == session.playerGuid()) {
        return false;
    }
    if (!leader) {
        error("ERR_GENERIC_NO_TARGET");
        return false;
    }
    const game::Reactions* reactions = m_reactions;
    if (!isPlayer(*leader) || !canCooperate(*me, *leader, reactions)) {
        error("ERR_INVALID_FOLLOW_TARGET");
        return false;
    }
    if (game::isDead(*me)) {
        error("ERR_PLAYER_DEAD");
        return false;
    }
    if ((me->value(uf::UNIT_FIELD_FLAGS) & kUnitFlagStunned) != 0 ||
        (me->value(uf::UNIT_FIELD_FLAGS) & game::kUnitFlagTaxiFlight) != 0) {
        error("ERR_TOOBUSYTOFOLLOW");
        return false;
    }
    const u32 now = session.lastUpdateMs();
    if (distance3(me->positionAt(now), leader->positionAt(now)) > game::kInteractDistances[4]) {
        error("ERR_AUTOFOLLOW_TOO_FAR");
        return false;
    }
    m_follow.guid = guid;
    m_follow.moving = true;
    m_follow.endedMs = 0;
    m_follow.shown = true;
    text::format(m_follow.status, "AUTOFOLLOWSTART", nameOf(session, guid)); // AUTOFOLLOW_BEGIN
    return true;
}

void UnitMenuScreens::stopFollow(u32 nowMs, bool show) {
    if (m_follow.guid == 0) {
        return;
    }
    m_follow.guid = 0;
    m_follow.moving = false;
    if (show) {
        // AUTOFOLLOW_END: "You stop following %s." with the name it began with.
        char name[64] = {};
        const std::string_view start = text::get("AUTOFOLLOWSTART");
        const usize at = start.find("%s");
        std::string_view status = m_follow.status.data();
        if (at != std::string_view::npos && status.size() >= start.size() - 2) {
            const usize length = status.size() - (start.size() - 2);
            std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(length), status.data() + at);
        }
        text::format(m_follow.status, "AUTOFOLLOWSTOP", std::string_view(name));
        m_follow.endedMs = std::max(nowMs, 1u);
    } else {
        m_follow.shown = false;
    }
}

void UnitMenuScreens::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    m_reactions = hud.reactions;
    if (!session.inWorld()) {
        m_follow.guid = 0;
        return;
    }
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        m_eventCursor = session.play().events().end();
        reset();
    }
    readEvents(session);
    updateFollow(session, hud.player, nowMs);
    updateInspect(session);
}

void UnitMenuScreens::readEvents(GameSession& session) {
    while (const game::GameEvent* e = session.play().events().next(m_eventCursor)) {
        switch (e->kind) {
        case game::GameEventKind::TimePlayed: {
            // ChatFrame_DisplayTimePlayed: TIME_PLAYED_TOTAL and TIME_PLAYED_LEVEL in the chat's system colour.
            if (!m_social) {
                break;
            }
            const auto line = [&](const char* key, u32 seconds) {
                char time[96], out[160];
                const u32 days = seconds / 86400, hours = seconds % 86400 / 3600, minutes = seconds % 3600 / 60;
                const std::string_view span =
                    text::format(time, "TIME_DAYHOURMINUTESECOND", days, hours, minutes, seconds % 60);
                m_social->systemNotice(rgba(255, 255, 0), text::format(out, key, span));
            };
            line("TIME_PLAYED_TOTAL", static_cast<u32>(e->value[0]));
            line("TIME_PLAYED_LEVEL", static_cast<u32>(e->value[1]));
            break;
        }
        case game::GameEventKind::PetNameInvalid: error("ERR_INVALID_PETNAME"); break; // WoW.exe 0x5e3e33
        case game::GameEventKind::PvpToggled:
            message(rgba(255, 255, 0), text::get(e->value[0] != 0 ? "ERR_PVP_TOGGLE_ON" : "ERR_PVP_TOGGLE_OFF"));
            break;
        default: break;
        }
    }
}

void UnitMenuScreens::updateFollow(GameSession& session, PlayerController* player, u32 nowMs) {
    VD_PROFILE_ZONE("UnitMenuScreens::follow");
    m_nowMs = nowMs;
    if (m_follow.guid == 0) {
        return;
    }
    if (!session.inWorld() || !player) {
        stopFollow(nowMs, false);
        return;
    }
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    const Entity* leader = entities.find(m_follow.guid);
    const u32 now = session.lastUpdateMs();
    // Any movement of the player's own, the leader gone or beyond the 30 yards, death, a taxi: it ends.
    if (player->takeFollowBroken() || !me || !leader || game::isDead(*me) || player->onServerSpline() ||
        (me->value(uf::UNIT_FIELD_FLAGS) & game::kUnitFlagTaxiFlight) != 0 ||
        distance3(me->positionAt(now), leader->positionAt(now)) > game::kInteractDistances[4]) {
        stopFollow(nowMs, true);
        return;
    }
    const glm::dvec3 feet = player->character().position();
    const glm::vec3 to = leader->positionAt(now) - glm::vec3(feet);
    const f32 flat = std::sqrt(to.x * to.x + to.y * to.y);
    m_follow.moving = m_follow.moving ? flat > kFollowNear : flat > kFollowResume;
    player->setFollowStep(m_follow.moving, std::atan2(to.y, to.x));
}

void UnitMenuScreens::drawFollowStatus(DialogContext& c, u32 nowMs) {
    if (!m_follow.shown) {
        return;
    }
    f32 alpha = 1.0f;
    if (m_follow.endedMs != 0) {
        const u32 since = nowMs - m_follow.endedMs;
        if (since >= kFollowFadeMs) {
            m_follow.shown = false;
            return;
        }
        alpha = 1.0f - static_cast<f32>(since) / static_cast<f32>(kFollowFadeMs);
    }
    autoFollowStatus(c, m_follow.status.data(), alpha);
}

bool UnitMenuScreens::initiateTrade(GameSession& session, u64 guid) {
    game::Trade& trade = session.play().exchange().trade();
    const std::string_view problem = trade.initiateProblem(guid);
    if (!problem.empty()) {
        message(kErrorColor, problem);
        return false;
    }
    return trade.initiate(guid);
}

bool UnitMenuScreens::inspectUnit(GameSession& session, u64 guid, bool fromTarget) {
    // CanInspect (WoW.exe 0x48a1b0): no unit, not a player of the player's side, beyond 10 yards.
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(session.playerGuid());
    const Entity* other = guid ? entities.find(guid) : nullptr;
    m_inspect = 0;
    if (!me) {
        return false;
    }
    if (!other) {
        error("ERR_GENERIC_NO_TARGET");
        return false;
    }
    const game::Reactions* reactions = m_reactions;
    if (!isPlayer(*other) || (other->guid != me->guid && !canCooperate(*me, *other, reactions))) {
        error("ERR_INVALID_INSPECT_TARGET");
        return false;
    }
    const u32 now = session.lastUpdateMs();
    if (distance3(me->positionAt(now), other->positionAt(now)) > game::kInteractDistances[1]) {
        error("ERR_OUT_OF_RANGE");
        return false;
    }
    session.play().commands().inspect(guid); // NotifyInspect
    m_inspect = guid;
    m_inspectTarget = fromTarget;
    return true;
}

// --- The InspectFrame ------------------------------------------------------------------------------------------

bool UnitMenuScreens::inspectOpen(const GameSession& session) const {
    return m_inspect != 0 && session.inWorld();
}

void UnitMenuScreens::updateInspect(GameSession& session) {
    if (m_inspect == 0) {
        return;
    }
    const u64 target = session.target();
    if (m_inspectTarget && target != m_inspect) {
        // InspectFrame_OnEvent: PLAYER_TARGET_CHANGED inspects the new target ("target" is the unit), and
        // InspectFrame_OnUpdate closes it without one.
        if (target == 0 || !inspectUnit(session, target, true)) {
            m_inspect = 0;
        }
    }
    if (!session.entities().find(m_inspect) || target == 0) {
        m_inspect = 0;
    }
}

void UnitMenuScreens::drawInspect(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("UnitMenuScreens::inspect");
    (void)nowMs;
    const Entity* other = m_inspect ? session.entities().find(m_inspect) : nullptr;
    if (!other) {
        return;
    }
    // InspectPaperDollItemSlotButton_Update: GetInventoryItemTexture(unit, slot) of the visible items.
    for (u32 slot = 0; slot < 19; ++slot) {
        const u16 base = static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride);
        const u32 entry = other->value(base);
        ItemSlotView& cell = m_inspectSlots[slot];
        cell = ItemSlotView{};
        cell.ref = {ItemRef::Kind::Window, 0, 0, slot};
        cell.emptyArt = InventoryScreens::equipmentSlotArt(slot);
        m_inspectEntries[slot] = entry;
        if (entry != 0) {
            cell.empty = false;
            if (const net::ItemQueryInfo* info = session.item(entry)) {
                cell.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
                cell.quality = info->quality;
            }
        }
    }
    ItemSlotView& ammo = m_inspectSlots[19]; // InspectPaperDollFrame has none: hidden by the view
    ammo = ItemSlotView{};
    m_inspectEntries[19] = 0;

    PaperDollView view;
    const std::string_view name = nameOf(session, m_inspect);
    view.title = copyName(m_inspectTitle, name);
    const u8 race = other->byteValue(uf::UNIT_FIELD_BYTES_0, 0), cls = other->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    m_items = m_inventory ? m_inventory->itemData() : nullptr; // ChrRaces / ChrClasses' names
    view.level = text::format(m_inspectSubtitle, "PLAYER_LEVEL", other->value(uf::UNIT_FIELD_LEVEL),
                              m_items ? m_items->raceName(race) : std::string_view{},
                              m_items ? m_items->className(cls) : std::string_view{});
    view.equipment = m_inspectSlots;
    view.ammoSlot = false; // InspectPaperDollFrame has none
    m_inspectDrag = ItemDrag{};
    // Phase 15 (character): app/CharacterScreens adds the model and the HONOR tab (InspectHonorFrame).
    const PaperDollResult result =
        m_sheet ? m_sheet->inspectWindow(session, c, m_inspect, view, m_inspectDrag) : paperDollWindow(c, view, m_inspectDrag);
    const ItemWindowResult& r = result.items;
    m_inspectDrag = ItemDrag{}; // nothing is carried out of someone else's equipment
    if (r.tapped.kind == ItemRef::Kind::Window && r.tapped.index < 19) {
        const u32 slot = r.tapped.index;
        const u32 entry = m_inspectEntries[slot];
        const u16 base = static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride);
        if (entry != 0 && m_inventory) {
            // GetInventoryItemLink(InspectFrame.unit, id): its permanent enchantment and random property.
            game::ItemInstance instance;
            instance.entry = entry;
            instance.enchantments[0] = other->value(static_cast<u16>(base + 1));
            instance.randomPropertyId = other->value(static_cast<u16>(base + 8));
            if (!m_inventory->insertItemLink(session, c, instance)) {
                m_inventory->showItemTooltip(entry, r.tappedRect, &instance);
            }
        }
    }
    if (r.closed) {
        m_inspect = 0;
    }
}

// --- Gallery (development) ---------------------------------------------------------------------------------------

void UnitMenuScreens::galleryMenu(DialogContext& c, u32 page, f32 x, f32 y) {
    // A party member's menu as its leader sees it, with the raid target icons open (page 0); the player's own in a
    // group led by someone else (page 1); a creature's icons (page 2). Texts by key; the rules are game/UnitPopup's.
    game::UnitPopupState s;
    s.inParty = true;
    s.leader = page != 1;
    s.canCooperate = true;
    s.lootMethod = 3;
    s.lootThreshold = 2;
    s.targetExists = true;
    s.targetIsPlayer = page != 2;
    s.targetCanCooperate = true;
    s.unitRaidTarget = 8;
    s.targetRaidTarget = page == 2 ? 3 : 8;
    s.inDistance = {true, true, false, true, true};
    const UnitPopupMenu menu = page == 1 ? UnitPopupMenu::Self : page == 2 ? UnitPopupMenu::RaidTargetIcon
                                                                          : UnitPopupMenu::Party;
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> items{};
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> sub{};
    const u32 count = game::unitPopupItems(menu, s, items);
    const u32 subCount = page == 0 ? game::unitPopupSubItems(B::RaidTargetIcon, s, sub) : 0;
    std::array<DropDownItemView, game::kUnitPopupMaxItems + 1> views{};
    std::array<DropDownItemView, game::kUnitPopupMaxItems> subViews{};
    views[0].text = page == 2 ? text::get("RAID_TARGET_ICON") : page == 1 ? "Arthel" : "Aeryn";
    views[0].title = true;
    i32 nestedRow = -1;
    const auto fill = [&](const game::UnitPopupItem& item, DropDownItemView& v) {
        const game::UnitPopupButtonInfo& info = game::unitPopupButtonInfo(item.button);
        v.text = item.button == B::LootMethod      ? text::get(game::lootMethodKey(s.lootMethod))
                 : item.button == B::LootThreshold ? text::get(game::itemQualityKey(s.lootThreshold))
                                                   : text::get(text::Key(info.text));
        v.color = item.button == B::LootThreshold ? ui::qualityColor(s.lootThreshold) : info.color;
        v.enabled = item.enabled;
        v.checkable = item.checkable;
        v.checked = item.checked;
        v.hasArrow = item.nested;
        v.raidIcon = info.icon;
    };
    for (u32 i = 0; i < count; ++i) {
        fill(items[i], views[i + 1]);
        if (items[i].button == B::RaidTargetIcon && page == 0) {
            nestedRow = static_cast<i32>(i + 1);
        }
    }
    for (u32 i = 0; i < subCount; ++i) {
        fill(sub[i], subViews[i]);
    }
    DropDownMenuView view;
    view.x = x;
    view.y = y;
    view.level1 = {views.data(), count + 1};
    view.level2 = {subViews.data(), subCount};
    view.level2Row = nestedRow;
    dropDownMenu(c, "unitMenu.gallery", view);
}

} // namespace vd
