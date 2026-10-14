#include "app/PopupScreens.h"

#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/ItemDefs.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26); // UI_ERROR_MESSAGE
constexpr i32 kReleaseMs = 6 * 60 * 1000;     // CORPSE_REPOP_TIME: the auto release (GetReleaseTimeRemaining)
constexpr i32 kResurrectAnswerMs = 60 * 1000; // RESURRECT: timeleft = the corpse delay + 60

// StaticPopup_OnUpdate's time left: seconds under a minute, minutes (rounded up) beyond; GetText's plural
// (_P1) unless it is one.
struct Countdown {
    u32 n = 0;
    std::string_view unit;
};
Countdown countdown(i32 ms) {
    const u32 seconds = ms <= 0 ? 0u : static_cast<u32>((ms + 999) / 1000);
    if (seconds < 60) {
        return {seconds, text::get(seconds == 1 ? "SECONDS" : "SECONDS_P1")};
    }
    const u32 minutes = (seconds + 59) / 60;
    return {minutes, text::get(minutes == 1 ? "MINUTES" : "MINUTES_P1")};
}

f32 floatValue(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

u64 packPos(game::ItemPos pos) {
    return (static_cast<u64>(pos.bag) << 8) | pos.slot;
}
game::ItemPos unpackPos(u64 data) {
    return {static_cast<u8>(data >> 8), static_cast<u8>(data)};
}

const char* kindName(PopupKind kind) {
    static constexpr const char* kNames[] = {"DEATH",
                                             "RESURRECT",
                                             "RESURRECT_NO_SICKNESS",
                                             "RESURRECT_NO_TIMER",
                                             "RECOVER_CORPSE",
                                             "XP_LOSS",
                                             "XP_LOSS_NO_SICKNESS",
                                             "PARTY_INVITE",
                                             "GUILD_INVITE",
                                             "DUEL_REQUESTED",
                                             "DUEL_OUTOFBOUNDS",
                                             "TRADE",
                                             "TRADE_WAITING",
                                             "QUEST_ACCEPT",
                                             "ABANDON_QUEST",
                                             "DELETE_ITEM",
                                             "DELETE_GOOD_ITEM",
                                             "CONFIRM_BINDER",
                                             "CONFIRM_TALENT_WIPE",
                                             "CONFIRM_SUMMON",
                                             "CAMP",
                                             "QUIT",
                                             "LOOT_BIND",
                                             "EQUIP_BIND",
                                             "AUTOEQUIP_BIND",
                                             "USE_BIND",
                                             "ABANDON_PET",
                                             "CONFIRM_BUY_BANK_SLOT",
                                             "CONFIRM_RESET_INSTANCES",
                                             "RECOVER_CORPSE_INSTANCE",
                                             "INSTANCE_BOOT",
                                             "BIND_ENCHANT", // phase 13 (spells)
                                             "REPLACE_ENCHANT",
                                             "TRADE_REPLACE_ENCHANT",
                                             "CONFIRM_GUILD_DISBAND",
                                             "CONFIRM_GUILD_PROMOTE",
                                             "CONFIRM_GUILD_LEAVE",
                                             "RENAME_PET",
                                             "PETRENAMECONFIRM",
                                             "CONFIRM_PET_UNLEARN", // phase 15 (pets)
                                             "CONFIRM_LOOT_ROLL", // phase 15 (items)
                                             "CONFIRM_LOOT_DISTRIBUTION",
                                             "UNLEARN_SKILL", // phase 15 (character)
                                             "ADD_GUILDMEMBER", // phase 15 (guild)
                                             "REMOVE_GUILDMEMBER",
                                             "ADD_GUILDRANK",
                                             "SET_GUILDMOTD",
                                             "SET_GUILDPLAYERNOTE",
                                             "SET_GUILDOFFICERNOTE",
                                             "RENAME_GUILD",
                                             "HELP_TICKET", // phase 15 (help)
                                             "HELP_TICKET_ABANDON_CONFIRM",
                                             "HELP_TICKET_QUEUE_DISABLED",
                                             "ADD_RAIDMEMBER", // phase 16 (raidframe)
                                             "CONFIRM_LEAVE_QUEUE", // phase 16 (alerts)
                                             "AREA_SPIRIT_HEAL",    // phase 17 (honor)
                                             "CONFIRM_BATTLEFIELD_ENTRY", // phase 17 (queue)
                                             "ABANDON_QUEST_WITH_ITEMS",  // phase 18 (p3)
                                             "DELETE_MAIL",
                                             "DELETE_MONEY",
                                             "NAME_CHAT",
                                             "COD_ALERT", // retouches (windows)
                                             "COD_CONFIRMATION",
                                             "ADD_FRIEND", // retouches (friends frame)
                                             "ADD_IGNORE"};
    static_assert(std::size(kNames) == static_cast<usize>(PopupKind::Count));
    return kind < PopupKind::Count ? kNames[static_cast<u32>(kind)] : "?";
}

const char* endName(PopupEnd end) {
    switch (end) {
    case PopupEnd::Accept: return "accept";
    case PopupEnd::Cancel: return "cancel";
    case PopupEnd::Override: return "override";
    case PopupEnd::Timeout: return "timeout";
    case PopupEnd::Refused: return "refused";
    case PopupEnd::Hidden: return "hidden";
    }
    return "?";
}

ui::WidgetId editId(u32 slot) {
    return ui::widgetId("popup.edit", slot);
}

// The popups with an edit box, and its maxLetters (DELETE_GOOD_ITEM: 32; phase 13: RENAME_PET's 12).
bool hasEdit(PopupKind kind) {
    switch (kind) {
    case PopupKind::DeleteGoodItem:
    case PopupKind::RenamePet:
    case PopupKind::AddGuildMember: // phase 15 (guild)
    case PopupKind::AddGuildRank:
    case PopupKind::SetGuildMotd:
    case PopupKind::SetGuildPlayerNote:
    case PopupKind::SetGuildOfficerNote:
    case PopupKind::RenameGuild:
    case PopupKind::AddRaidMember: // phase 16 (raidframe)
    case PopupKind::AddFriend:     // retouches (friends frame)
    case PopupKind::AddIgnore: return true;
    case PopupKind::NameChat: return true;      // phase 18 (p3)
    default: return false;
    }
}
u32 editLetters(PopupKind kind) {
    switch (kind) {
    case PopupKind::RenamePet:
    case PopupKind::AddGuildMember: return 12u; // phase 15 (guild): maxLetters of each
    case PopupKind::AddRaidMember: return 12u;  // phase 16 (raidframe)
    case PopupKind::AddFriend:                  // retouches (friends frame)
    case PopupKind::AddIgnore: return 12u;
    case PopupKind::AddGuildRank: return 15u;
    case PopupKind::NameChat: return 31u; // phase 18 (p3)
    case PopupKind::SetGuildMotd: return 128u;
    case PopupKind::SetGuildPlayerNote:
    case PopupKind::SetGuildOfficerNote: return 31u;
    case PopupKind::RenameGuild: return 24u;
    default: return 32u;
    }
}

} // namespace

void PopupScreens::init(const game::GameData* data) {
    m_data = data;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
}

void PopupScreens::leaveWorld() {
    m_cursorsPlayer = 0;
    reset();
}

void PopupScreens::reset() {
    m_stack.clear();
    PopupOutcome discarded;
    while (m_stack.takeOutcome(discarded)) {
    }
    m_wasDeadBody = false;
    m_diedAtMs = 0;
    m_lastResurrector = 0;
    m_corpseInRange = false;
    m_corpseInInstance = false;
    m_lastSpiritHealer = 0;
    m_tradeProposed = false;
    m_bind = PendingBind{};
    m_editLength = 0;
    m_editText[0] = '\0';
    m_focusEdit = false;
}

void PopupScreens::message(u32 color, std::string_view line) {
    if (m_message) {
        char buffer[256];
        const usize n = std::min(line.size(), sizeof(buffer) - 1);
        std::memcpy(buffer, line.data(), n);
        buffer[n] = '\0';
        m_message(m_messageContext, color, false, buffer);
    }
}

std::string_view PopupScreens::playerName(GameSession& session, u64 guid) {
    const std::string* name = guid != 0 ? session.playerName(guid) : nullptr;
    return name && !name->empty() ? std::string_view(*name) : std::string_view("...");
}

bool PopupScreens::nearUnit(GameSession& session, u64 guid, u32 nowMs) {
    // CheckBinderDist, CheckTalentMasterDist, CheckSpiritHealerDist: within the interaction distance (the
    // server measures between the bounding spheres).
    const Entity* me = session.entities().find(session.playerGuid());
    const Entity* other = guid != 0 ? session.entities().find(guid) : nullptr;
    if (!me || !other) {
        return false;
    }
    const f32 gap = game::distanceAt(*me, *other, nowMs) - floatValue(*other, uf::UNIT_FIELD_BOUNDINGRADIUS) -
                    floatValue(*me, uf::UNIT_FIELD_BOUNDINGRADIUS);
    return gap <= game::kInteractionDistance + 1.0f;
}

Popup* PopupScreens::show(GameSession& session, PopupKind kind, u64 data) {
    const game::DeathState& death = session.play().death();
    Popup* p = m_stack.show(kind, death.dead, data);
    if (p) {
        m_uiSound(UiSound::GameMenuOpen); // StaticPopup_OnShow
        if (hasEdit(kind)) {
            m_editLength = 0;
            m_editText[0] = '\0';
            m_focusEdit = true;
        }
    }
    if (m_trace) {
        VD_LOG_INFO("Popup %s %s (data %llx)", kindName(kind), p ? "shown" : "refused",
                    static_cast<unsigned long long>(data));
    }
    return p;
}

// --- Questions the windows ask -----------------------------------------------------------------------------

void PopupScreens::askAbandonQuest(GameSession& session, u32 questId) {
    // Phase 18 (p3): QuestLogFrameAbandonButton's OnClick: SetAbandonQuest, then ABANDON_QUEST_WITH_ITEMS (with the
    // items GetAbandonQuestItems names) or ABANDON_QUEST, the other one hidden.
    if (!abandonQuestItems(session, questId, m_abandonItems).empty()) {
        m_stack.hide(PopupKind::AbandonQuest);
        show(session, PopupKind::AbandonQuestWithItems, questId);
        return;
    }
    m_stack.hide(PopupKind::AbandonQuestWithItems);
    show(session, PopupKind::AbandonQuest, questId);
}

std::string_view PopupScreens::abandonQuestItems(GameSession& session, u32 questId, std::string& out) {
    out.clear();
    game::Gameplay& play = session.play();
    const game::QuestInfo* quest = play.quest(questId);
    if (!quest) {
        return out;
    }
    constexpr u32 kBindQuestItem = 4, kBindQuestItem1 = 5; // ItemBondingType
    std::array<u32, net::kQuestObjectiveCount> items{};
    u32 count = 0;
    for (const game::QuestInfo::Objective& o : quest->objectiveList) {
        if (o.itemId == 0 || play.inventory().count(o.itemId) == 0) {
            continue;
        }
        const net::ItemQueryInfo* info = session.item(o.itemId);
        if (info && (info->bonding == kBindQuestItem || info->bonding == kBindQuestItem1) && count < items.size()) {
            items[count++] = o.itemId;
        }
    }
    // What another quest in the log needs too stays.
    for (const game::QuestLogEntry& entry : play.questLog().slots()) {
        if (entry.questId == 0 || entry.questId == questId) {
            continue;
        }
        if (const game::QuestInfo* other = play.quest(entry.questId)) {
            for (const game::QuestInfo::Objective& o : other->objectiveList) {
                for (u32& item : items) {
                    if (o.itemId != 0 && item == o.itemId) {
                        item = 0;
                    }
                }
            }
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if (items[i] == 0) {
            continue;
        }
        const net::ItemQueryInfo* info = session.item(items[i]);
        if (!out.empty()) {
            out.append(", ");
        }
        out.append(info ? std::string_view(info->name) : std::string_view("..."));
    }
    return out;
}

void PopupScreens::askNameChat(GameSession& session, u32 chatFrame) {
    show(session, PopupKind::NameChat, chatFrame);
}

void PopupScreens::askDeleteMail(GameSession& session, u32 mailId, std::string_view itemName) {
    if (Popup* p = show(session, PopupKind::DeleteMail, mailId)) {
        p->setName(itemName);
    }
}

void PopupScreens::askDeleteMoney(GameSession& session, u32 mailId, u32 copper) {
    if (Popup* p = show(session, PopupKind::DeleteMoney, mailId)) {
        p->money = copper; // OnShow: MoneyFrame_Update with OpenMailFrame.money
    }
}

void PopupScreens::askCod(GameSession& session, u32 mailId, u32 cod) {
    if (cod > session.play().inventory().money()) {
        show(session, PopupKind::CodAlert);
    } else if (Popup* p = show(session, PopupKind::CodConfirmation, mailId)) {
        p->money = cod; // OnShow: MoneyFrame_Update with OpenMailFrame.cod
    }
}

void PopupScreens::askDestroyItem(GameSession& session, game::ItemPos pos) {
    const game::ItemInstance* item = session.play().inventory().at(pos);
    if (!item || item->empty()) {
        return;
    }
    // DELETE_ITEM_CONFIRM: DELETE_GOOD_ITEM for rare and better (the quality UIParent checks, arg2 >= 3).
    const net::ItemQueryInfo* info = session.item(item->entry);
    const bool good = info && info->quality >= 3;
    if (Popup* p = show(session, good ? PopupKind::DeleteGoodItem : PopupKind::DeleteItem, packPos(pos))) {
        p->data2 = item->guid;
        p->button1Enabled = !good;
    }
}

bool PopupScreens::askBindForAction(GameSession& session, const game::ItemPlace& place,
                                    game::ItemAction action) {
    const game::BindAsk ask = game::itemActionBindAsk(session.play(), place, action);
    if (ask == game::BindAsk::None) {
        return false;
    }
    m_bind = PendingBind{};
    m_bind.ask = ask;
    m_bind.from = place;
    m_bind.action = action;
    show(session, ask == game::BindAsk::Use ? PopupKind::UseBind : PopupKind::AutoEquipBind, ++m_bindSerial);
    return true;
}

bool PopupScreens::askBindForDrop(GameSession& session, const game::ItemPlace& from,
                                  const game::ItemPlace& to) {
    const game::BindAsk ask = game::dropBindAsk(session.play(), from, to);
    if (ask == game::BindAsk::None) {
        return false;
    }
    m_bind = PendingBind{};
    m_bind.ask = ask;
    m_bind.from = from;
    m_bind.to = to;
    // UIParent: EQUIP_BIND_CONFIRM hides AUTOEQUIP_BIND and the other way round (both exclusive anyway).
    show(session, ask == game::BindAsk::AutoEquip ? PopupKind::AutoEquipBind : PopupKind::EquipBind,
         ++m_bindSerial);
    return true;
}

bool PopupScreens::askBindForUse(GameSession& session, u32 itemEntry) {
    if (game::useItemBindAsk(session.play(), itemEntry) == game::BindAsk::None) {
        return false;
    }
    m_bind = PendingBind{};
    m_bind.ask = game::BindAsk::Use;
    m_bind.itemEntry = itemEntry;
    show(session, PopupKind::UseBind, ++m_bindSerial);
    return true;
}

bool PopupScreens::askBindForLoot(GameSession& session, u8 lootSlot, u32 itemEntry) {
    if (game::lootBindAsk(session.play(), itemEntry) == game::BindAsk::None) {
        return false;
    }
    m_bind = PendingBind{};
    m_bind.ask = game::BindAsk::Loot;
    m_bind.lootSlot = lootSlot;
    m_bind.itemEntry = itemEntry;
    m_bind.lootGuid = session.play().loot().guid();
    show(session, PopupKind::LootBind, ++m_bindSerial);
    return true;
}

bool PopupScreens::askLootRoll(GameSession& session, u64 object, u32 slot, u8 vote, u32 itemEntry) {
    // RollOnLoot: the client asks before Need or Greed on what binds when picked up (the server never does).
    const net::ItemQueryInfo* info = session.item(itemEntry);
    if (vote == net::kRollPass || !info || info->bonding != game::kBindOnPickup) {
        return false;
    }
    if (Popup* p = show(session, PopupKind::ConfirmLootRoll, object)) { // UIParent's CONFIRM_LOOT_ROLL
        p->data2 = u64{slot} | (u64{vote} << 32);
    }
    return true;
}

bool PopupScreens::askLootDistribution(GameSession& session, u8 lootSlot, u32 quality, std::string_view itemName,
                                       u64 candidate, std::string_view candidateName) {
    constexpr u32 kMasterLootThreshold = 4; // LootFrame.lua's MASTER_LOOT_THREHOLD
    if (quality < kMasterLootThreshold) {
        return false;
    }
    m_stack.hide(PopupKind::ConfirmLootDistribution);
    if (Popup* p = show(session, PopupKind::ConfirmLootDistribution, candidate)) {
        // ITEM_QUALITY_COLORS[quality].hex..name..FONT_COLOR_CODE_CLOSE, then the candidate's name.
        char colored[64];
        const std::string_view color = game::itemQualityColor(static_cast<u8>(quality));
        std::snprintf(colored, sizeof(colored), "%.*s%.*s|r", static_cast<int>(color.size()), color.data(),
                      static_cast<int>(itemName.size()), itemName.data());
        p->setName(colored);
        p->setName2(candidateName);
        p->data2 = lootSlot | (session.play().loot().guid() << 8);
    }
    return true;
}

void PopupScreens::askAbandonPet(GameSession& session) {
    show(session, PopupKind::AbandonPet);
}

void PopupScreens::askResetInstances(GameSession& session) {
    show(session, PopupKind::ConfirmResetInstances);
}

void PopupScreens::askLeaveMeetingStoneQueue(GameSession& session) { // phase 16 (alerts)
    show(session, PopupKind::ConfirmLeaveQueue);
}

// Phase 17 (queue).
void PopupScreens::askBattlefieldEntry(GameSession& session, u32 slot, std::string_view mapName) {
    if (Popup* p = show(session, PopupKind::ConfirmBattlefieldEntry, slot)) {
        p->setName(mapName); // dialog.data = i
    }
}

void PopupScreens::hideBattlefieldEntry(u32 slot) {
    m_stack.hide(PopupKind::ConfirmBattlefieldEntry, slot); // StaticPopup_Hide(..., i)
}

void PopupScreens::askGuildDisband(GameSession& session) {
    show(session, PopupKind::ConfirmGuildDisband);
}

void PopupScreens::askGuildPromote(GameSession& session, std::string_view name) {
    if (Popup* p = show(session, PopupKind::ConfirmGuildPromote)) {
        p->setName(name); // dialog.data = name
    }
}

void PopupScreens::askGuildLeave(GameSession& session) {
    const game::GuildState& guild = session.play().social().guild();
    if (Popup* p = show(session, PopupKind::ConfirmGuildLeave)) {
        p->setName(guild.known ? std::string_view(guild.name) : text::get("GUILD")); // GetGuildInfo("player")
    }
}

void PopupScreens::askRenamePet(GameSession& session) {
    show(session, PopupKind::RenamePet, session.play().abilities().pet().guid);
}

void PopupScreens::askUnlearnSkill(GameSession& session, u32 skill, std::string_view name) {
    if (Popup* p = show(session, PopupKind::UnlearnSkill, skill)) { // phase 15 (character)
        p->setName(name);
    }
}

void PopupScreens::hideUnlearnSkill(u32 selectedSkill) {
    const Popup* p = m_stack.find(PopupKind::UnlearnSkill);
    if (p && p->data != selectedSkill) {
        m_stack.hide(PopupKind::UnlearnSkill);
    }
}

// Phase 15 (guild, help).

void PopupScreens::prefillEdit(std::string_view text) {
    m_editLength = static_cast<u32>(std::min(text.size(), m_editText.size() - 1));
    std::memcpy(m_editText.data(), text.data(), m_editLength);
    m_editText[m_editLength] = '\0';
}

void PopupScreens::askAddGuildMember(GameSession& session) {
    show(session, PopupKind::AddGuildMember);
}

void PopupScreens::askAddRaidMember(GameSession& session) { // phase 16 (raidframe)
    show(session, PopupKind::AddRaidMember);
}

void PopupScreens::askAddFriend(GameSession& session) { // retouches (friends frame)
    show(session, PopupKind::AddFriend);
}

void PopupScreens::askAddIgnore(GameSession& session) {
    show(session, PopupKind::AddIgnore);
}

void PopupScreens::askRemoveGuildMember(GameSession& session, std::string_view name) {
    if (Popup* p = show(session, PopupKind::RemoveGuildMember)) {
        p->setName(name); // GuildFrame.selectedName
    }
}

void PopupScreens::askAddGuildRank(GameSession& session) {
    show(session, PopupKind::AddGuildRank);
}

void PopupScreens::askGuildMotd(GameSession& session, std::string_view motd) {
    if (show(session, PopupKind::SetGuildMotd)) {
        prefillEdit(motd.substr(0, editLetters(PopupKind::SetGuildMotd))); // OnShow: CURRENT_GUILD_MOTD
    }
}

void PopupScreens::askGuildNote(GameSession& session, std::string_view member, std::string_view note, bool officer) {
    const PopupKind kind = officer ? PopupKind::SetGuildOfficerNote : PopupKind::SetGuildPlayerNote;
    if (Popup* p = show(session, kind)) {
        p->setName(member); // GetGuildRosterSelection()
        prefillEdit(note.substr(0, editLetters(kind)));
    }
}

void PopupScreens::askRenameGuild(GameSession& session) {
    show(session, PopupKind::RenameGuild);
}

void PopupScreens::askHelpTicket(GameSession& session) {
    show(session, PopupKind::HelpTicket);
}

void PopupScreens::askHelpQueueDisabled(GameSession& session) {
    show(session, PopupKind::HelpTicketQueueDisabled);
}

void PopupScreens::hideHelpTicket() {
    m_stack.hide(PopupKind::HelpTicketAbandonConfirm);
    m_stack.hide(PopupKind::HelpTicket);
}

bool PopupScreens::helpTicketShown() const {
    return m_stack.visible(PopupKind::HelpTicket) || m_stack.visible(PopupKind::HelpTicketAbandonConfirm);
}

void PopupScreens::askBuyBankSlot(GameSession& session) {
    const game::Inventory& inv = session.play().inventory();
    if (inv.banker() == 0) {
        return;
    }
    if (Popup* p = show(session, PopupKind::ConfirmBuyBankSlot, inv.banker())) {
        p->money = inv.nextBankSlotPrice(); // OnShow: MoneyFrame_Update(BankFrame.nextSlotCost)
    }
}

void PopupScreens::askLogout(GameSession& session, bool quit) {
    if (loggingOut()) {
        return; // GameMenuFrame disables Logout and Exit Game while CAMP or QUIT is up
    }
    m_uiSound(UiSound::Logout); // igMainMenuLogout (Exit Game: igMainMenuQuit, the menu's close)
    session.requestLogout(quit);
}

bool PopupScreens::escape(GameSession& session) {
    const bool closed = m_stack.escape();
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        answer(session, o);
    }
    return closed;
}

// --- What shows and hides them -----------------------------------------------------------------------------

void PopupScreens::showDeath(GameSession& session, u32 nowMs) {
    // PLAYER_DEAD: DEATH while GetReleaseTimeRemaining() > 0, or -1 (no auto release: instances).
    const game::DeathState& death = session.play().death();
    if (!death.dead || death.ghost || m_stack.visible(PopupKind::Death)) {
        return;
    }
    // PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW (vmangos defines it, never sets it): no DEATH at all.
    constexpr u8 kNoReleaseWindow = 0x10;
    const Entity* me = session.entities().find(session.playerGuid());
    if (me && (me->byteValue(uf::PLAYER_FIELD_BYTES, 0) & kNoReleaseWindow) != 0) {
        return;
    }
    const i32 left = kReleaseMs - static_cast<i32>(nowMs - m_diedAtMs);
    if (death.releaseTimer && left <= 0) {
        return;
    }
    if (Popup* p = show(session, PopupKind::Death)) {
        p->data2 = death.releaseTimer ? 1 : 0;
        p->timeLeftMs = death.releaseTimer ? left : 0;
        p->button2Shown = death.selfResSpell != 0;
    }
}

void PopupScreens::showResurrect(GameSession& session, u32 nowMs) {
    // RESURRECT_REQUEST: with sickness, with the corpse delay still running, or neither.
    game::Gameplay& play = session.play();
    const game::DeathState& death = play.death();
    if (death.resurrector == 0) {
        return;
    }
    const u32 delay = death.resurrectDelayed ? play.reclaimDelayRemaining(nowMs) : 0;
    const PopupKind kind = death.resurrectSickness ? PopupKind::Resurrect
                           : delay > 0             ? PopupKind::ResurrectNoSickness
                                                   : PopupKind::ResurrectNoTimer;
    if (Popup* p = show(session, kind, death.resurrector)) {
        p->setName(!death.resurrectorName.empty() ? std::string_view(death.resurrectorName)
                                                  : playerName(session, death.resurrector));
        p->startDelayMs = kind == PopupKind::ResurrectNoTimer ? 0 : static_cast<i32>(delay);
        p->timeLeftMs = static_cast<i32>(delay) + kResurrectAnswerMs;
    }
}

void PopupScreens::readEvents(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        using K = game::GameEventKind;
        switch (e->kind) {
        case K::QuestShared: // QUEST_ACCEPT_CONFIRM
            if (Popup* p = show(session, PopupKind::QuestAccept, e->id)) {
                p->data2 = e->guid;
            }
            break;
        case K::SpiritHealerConfirm: { // CONFIRM_XP_LOSS (and HideUIPanel(GossipFrame))
            play.closeDialog();
            const Entity* me = session.entities().find(session.playerGuid());
            const u32 level = me ? me->value(uf::UNIT_FIELD_LEVEL) : 1;
            // Resurrection sickness (Player::ResurrectUsingSpiritHealer, DeathSickness.Level 11): none up to
            // level 10, then a minute per level above it, ten from level 20.
            if (level <= 10) {
                show(session, PopupKind::XpLossNoSickness, e->guid);
            } else if (Popup* p = show(session, PopupKind::XpLoss, e->guid)) {
                const u32 minutes = std::min(level - 10, 10u);
                char duration[32];
                p->setName(text::format(duration, minutes == 1 ? "GENERIC_MIN" : "GENERIC_MIN_P1", minutes));
            }
            m_lastSpiritHealer = e->guid;
            break;
        }
        case K::BinderConfirm: show(session, PopupKind::ConfirmBinder, e->guid); break;
        case K::TalentWipeOffer:
            if (e->guid != 0) {
                if (Popup* p = show(session, PopupKind::ConfirmTalentWipe, e->guid)) {
                    p->money = static_cast<u32>(e->value[0]);
                }
            }
            break;
        case K::PetUnlearnOffer: // phase 15 (pets): UIParent's CONFIRM_PET_UNLEARN, the cost in its money frame
            if (Popup* p = show(session, PopupKind::ConfirmPetUnlearn, play.stable().unlearnTrainer())) {
                p->money = static_cast<u32>(e->value[0]);
            }
            break;
        case K::TradeRequested: show(session, PopupKind::Trade, e->guid); break;
        case K::TradeOpened:
        case K::TradeClosed: // TRADE_REQUEST_CANCEL / TRADE_SHOW
            m_stack.hide(PopupKind::Trade);
            m_stack.hide(PopupKind::TradeWaiting);
            break;
        case K::LogoutStarted: // PLAYER_CAMPING / PLAYER_QUITING
            show(session, e->value[0] != 0 ? PopupKind::Quit : PopupKind::Camp);
            break;
        case K::LogoutCancelled: // LOGOUT_CANCEL
            m_stack.hide(PopupKind::Camp);
            m_stack.hide(PopupKind::Quit);
            break;
        case K::LogoutRefused: message(kErrorColor, text::get("ERR_LOGOUT_FAILED")); break;
        case K::SummonRequested:
            if (Popup* p = show(session, PopupKind::ConfirmSummon, e->guid)) {
                p->data2 = e->id;
                p->timeLeftMs = std::max(1, e->value[0]); // GetSummonConfirmTimeLeft
            }
            break;
        case K::InstanceBootStarted: { // INSTANCE_BOOT_START; OnShow: GetInstanceBootTimeRemaining, gone at 0
            const u32 left = play.travel().bootRemainingMs(nowMs);
            if (left == 0) {
                m_stack.hide(PopupKind::InstanceBoot);
            } else if (Popup* p = show(session, PopupKind::InstanceBoot)) {
                p->timeLeftMs = static_cast<i32>(std::min<u32>(left, 0x7FFFFFFFu));
            }
            break;
        }
        case K::InstanceBootStopped: m_stack.hide(PopupKind::InstanceBoot); break; // INSTANCE_BOOT_STOP
        // Phase 17 (honor): AREA_SPIRIT_HEALER_IN_RANGE shows AREA_SPIRIT_HEAL (OnShow: timeleft =
        // GetAreaSpiritHealerTime, whole seconds; AcceptAreaSpiritHeal), _OUT_OF_RANGE hides it;
        // PLAYER_SKINNED hides the resurrection offers.
        case K::AreaSpiritHealerInRange:
            if (Popup* p = show(session, PopupKind::AreaSpiritHeal, e->guid)) {
                p->timeLeftMs = static_cast<i32>(play.areaSpiritHealer().timeLeftSeconds(nowMs) * 1000);
                play.areaSpiritHealer().accept();
            }
            break;
        case K::AreaSpiritHealerOutOfRange: m_stack.hide(PopupKind::AreaSpiritHeal); break;
        case K::PlayerSkinned:
            for (const PopupKind kind :
                 {PopupKind::Resurrect, PopupKind::ResurrectNoSickness, PopupKind::ResurrectNoTimer}) {
                m_stack.hide(kind);
            }
            break;
        // Phase 13 (spells): BIND_ENCHANT, REPLACE_ENCHANT (arg1 old, arg2 new), TRADE_REPLACE_ENCHANT; the
        // spell no longer waiting hides them (CURRENT_SPELL_CAST_CHANGED).
        case K::EnchantQuestion: {
            const u64 names = static_cast<u32>(e->value[1]) | static_cast<u64>(static_cast<u32>(e->value[2])) << 32;
            switch (static_cast<game::EnchantAsk>(e->value[0])) {
            case game::EnchantAsk::Bind: show(session, PopupKind::BindEnchant, e->guid); break;
            case game::EnchantAsk::Replace: show(session, PopupKind::ReplaceEnchant, names); break;
            case game::EnchantAsk::TradeReplace: show(session, PopupKind::TradeReplaceEnchant, names); break;
            default: break;
            }
            break;
        }
        case K::SpellTargetingStopped:
            m_stack.hide(PopupKind::BindEnchant);
            m_stack.hide(PopupKind::ReplaceEnchant);
            m_stack.hide(PopupKind::TradeReplaceEnchant);
            break;
        default: break;
        }
    }
}

void PopupScreens::readSocial(GameSession& session) {
    const game::Social& social = session.play().social();
    while (const game::SocialEvent* e = social.events().next(m_socialCursor)) {
        using K = game::SocialEventKind;
        switch (e->kind) {
        case K::GroupInvite: // PARTY_INVITE_REQUEST
            if (Popup* p = show(session, PopupKind::PartyInvite)) {
                p->setName(e->name);
            }
            break;
        case K::GuildInvite: // GUILD_INVITE_REQUEST
            if (Popup* p = show(session, PopupKind::GuildInvite)) {
                p->setName(e->name);
                p->setName2(e->text);
            }
            break;
        case K::DuelRequested: // DUEL_REQUESTED (not the player's own challenge)
            if (e->value[0] != 1) {
                show(session, PopupKind::DuelRequested, e->guid);
            }
            break;
        case K::DuelOutOfBounds: show(session, PopupKind::DuelOutOfBounds); break;
        case K::DuelInBounds: m_stack.hide(PopupKind::DuelOutOfBounds); break;
        case K::DuelComplete: // DUEL_FINISHED
            m_stack.hide(PopupKind::DuelRequested);
            m_stack.hide(PopupKind::DuelOutOfBounds);
            break;
        default: break;
        }
    }
}

void PopupScreens::watchState(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::DeathState& death = play.death();
    const Entity* me = session.entities().find(session.playerGuid());

    // Death: DEATH while the body lies there (PLAYER_DEAD), gone once released or alive (PLAYER_ALIVE); its
    // countdown may start a moment later than the death (the release timer flag), its second button follows
    // the self-resurrection field.
    const bool deadBody = death.dead && !death.ghost;
    if (deadBody && !m_wasDeadBody) {
        m_diedAtMs = nowMs;
        showDeath(session, nowMs);
    } else if (!deadBody && m_wasDeadBody) {
        m_stack.hide(PopupKind::Death);
    }
    m_wasDeadBody = deadBody;
    if (Popup* p = m_stack.find(PopupKind::Death)) {
        p->button2Shown = death.selfResSpell != 0;
        if (p->data2 == 0 && death.releaseTimer) {
            p->data2 = 1;
            p->timeLeftMs = std::max(1, kReleaseMs - static_cast<i32>(nowMs - m_diedAtMs));
        }
    }
    // A resurrection offer (RESURRECT_REQUEST); gone when alive (PLAYER_UNGHOST) or answered elsewhere.
    if (death.resurrector != 0 && death.resurrector != m_lastResurrector) {
        showResurrect(session, nowMs);
    }
    m_lastResurrector = death.resurrector;
    if (!death.dead || death.resurrector == 0) {
        for (const PopupKind kind :
             {PopupKind::Resurrect, PopupKind::ResurrectNoSickness, PopupKind::ResurrectNoTimer}) {
            m_stack.hide(kind);
        }
    }
    // The ghost by its corpse: CORPSE_IN_RANGE / CORPSE_OUT_OF_RANGE (RECOVER_CORPSE, with the corpse delay).
    // Phase 12: a corpse inside an instance is reported at the instance's entrance (MSG_CORPSE_QUERY); there
    // the client raises CORPSE_IN_INSTANCE instead (RECOVER_CORPSE_INSTANCE: walk in to be resurrected).
    bool near = false;
    if (death.ghost && death.corpseKnown && me && me->hasPosition &&
        static_cast<u32>(death.corpseMap) == session.map()) {
        const glm::vec3 at = me->positionAt(nowMs);
        const f32 dx = at.x - death.corpseX, dy = at.y - death.corpseY, dz = at.z - death.corpseZ;
        near = dx * dx + dy * dy + dz * dz <= game::kCorpseReclaimRadius * game::kCorpseReclaimRadius;
    }
    const bool inInstance = near && death.corpseRealMap != session.map();
    const bool inRange = near && !inInstance;
    if (inRange && !m_corpseInRange) {
        if (Popup* p = show(session, PopupKind::RecoverCorpse)) {
            p->startDelayMs = static_cast<i32>(play.reclaimDelayRemaining(nowMs));
        }
    } else if (!inRange && m_corpseInRange) {
        m_stack.hide(PopupKind::RecoverCorpse);
        m_stack.hide(PopupKind::XpLoss);
        m_stack.hide(PopupKind::XpLossNoSickness);
    }
    m_corpseInRange = inRange;
    if (inInstance && !m_corpseInInstance) {
        show(session, PopupKind::RecoverCorpseInstance);
    } else if (!inInstance && m_corpseInInstance) { // CORPSE_OUT_OF_RANGE hides it too
        m_stack.hide(PopupKind::RecoverCorpseInstance);
    }
    m_corpseInInstance = inInstance;
    if (!death.ghost) {
        m_stack.hide(PopupKind::RecoverCorpse);
        m_stack.hide(PopupKind::RecoverCorpseInstance);
    }
    // INSTANCE_BOOT: gone when its countdown is (stopped, or the world changed under it).
    if (m_stack.visible(PopupKind::InstanceBoot) && play.travel().bootRemainingMs(nowMs) == 0) {
        m_stack.hide(PopupKind::InstanceBoot);
    }

    // OnUpdate: the NPC questions close when the player walks away (CheckSpiritHealerDist, CheckBinderDist,
    // CheckTalentMasterDist), or when the question is gone.
    for (const PopupKind kind : {PopupKind::XpLoss, PopupKind::XpLossNoSickness, PopupKind::ConfirmBinder,
                                 PopupKind::ConfirmTalentWipe}) {
        const Popup* p = m_stack.find(kind);
        if (p && !nearUnit(session, p->data, nowMs)) {
            m_stack.hide(kind);
        }
    }
    if (play.travel().binderAsking() == 0) {
        m_stack.hide(PopupKind::ConfirmBinder);
    }
    if (play.abilities().talentWipeTrainer() == 0) {
        m_stack.hide(PopupKind::ConfirmTalentWipe);
    }
    // Phase 15 (pets): CONFIRM_PET_UNLEARN's OnUpdate (CheckPetUntrainerDist), or the offer gone (the pet too).
    if (const Popup* p = m_stack.find(PopupKind::ConfirmPetUnlearn);
        p && (play.stable().unlearnPet() == 0 || (p->data != 0 && !nearUnit(session, p->data, nowMs)))) {
        m_stack.hide(PopupKind::ConfirmPetUnlearn);
    }

    // Invitations the inviter withdrew (PARTY_INVITE_CANCEL, GUILD_INVITE_CANCEL), a challenge that ended.
    const game::Social& social = play.social();
    if (social.group().pendingInviter.empty()) {
        m_stack.hide(PopupKind::PartyInvite);
    }
    if (social.guild().pendingInviter.empty()) {
        m_stack.hide(PopupKind::GuildInvite);
    }
    if (social.duel().phase != game::DuelPhase::Challenged) {
        m_stack.hide(PopupKind::DuelRequested);
    }
    // Trade: someone's request until it is answered; the player's own until the other answers.
    const game::Trade& trade = play.exchange().trade();
    if (trade.phase() != game::TradePhase::Requested) {
        m_stack.hide(PopupKind::Trade);
    }
    const bool proposed = trade.phase() == game::TradePhase::Proposed;
    if (proposed && !m_tradeProposed) {
        show(session, PopupKind::TradeWaiting, trade.partner());
    } else if (!proposed && m_tradeProposed) {
        m_stack.hide(PopupKind::TradeWaiting);
    }
    m_tradeProposed = proposed;
    // CONFIRM_SUMMON: not while in combat (its OnUpdate disables the button).
    if (Popup* p = m_stack.find(PopupKind::ConfirmSummon)) {
        p->button1Enabled = !(me && game::inCombat(*me));
        if (play.summon().summoner(nowMs) == 0) {
            m_stack.hide(PopupKind::ConfirmSummon);
        }
    }
    // DELETE_ITEM's OnUpdate: gone when the item is (CursorHasItem).
    for (const PopupKind kind : {PopupKind::DeleteItem, PopupKind::DeleteGoodItem}) {
        if (const Popup* p = m_stack.find(kind)) {
            const game::ItemInstance* item = play.inventory().at(unpackPos(p->data));
            if (!item || item->guid != p->data2) {
                m_stack.hide(kind);
            }
        }
    }
    // CONFIRM_BUY_BANK_SLOT: the bank it was about closed.
    if (const Popup* p = m_stack.find(PopupKind::ConfirmBuyBankSlot);
        p && play.inventory().banker() != p->data) {
        m_stack.hide(PopupKind::ConfirmBuyBankSlot);
    }
    // LOOT_BIND: the loot it was about closed.
    if (m_stack.visible(PopupKind::LootBind) &&
        (!play.loot().open() || play.loot().guid() != m_bind.lootGuid)) {
        m_stack.hide(PopupKind::LootBind);
    }
    // Phase 15 (items): CONFIRM_LOOT_ROLL goes with its roll (CANCEL_LOOT_ROLL: decided, timed out, voted elsewhere);
    // CONFIRM_LOOT_DISTRIBUTION with the loot window.
    if (const Popup* p = m_stack.find(PopupKind::ConfirmLootRoll)) {
        bool pending = false;
        for (const game::PendingRoll& roll : play.exchange().rolls().rolls()) {
            pending = pending || (roll.object == p->data && roll.slot == (p->data2 & 0xFFFFFFFFu) && !roll.voted);
        }
        if (!pending) {
            m_stack.hide(PopupKind::ConfirmLootRoll);
        }
    }
    if (m_stack.visible(PopupKind::ConfirmLootDistribution) && !play.loot().open()) {
        m_stack.hide(PopupKind::ConfirmLootDistribution);
    }
    // CAMP / QUIT without a logout under way (lost, or answered by a LOGOUT_CANCEL the popup missed).
    if (!play.logout().pending()) {
        m_stack.hide(PopupKind::Camp);
        m_stack.hide(PopupKind::Quit);
    }
}

void PopupScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("Popups::update");
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        // A new character in the world: from now on, with nothing left of the last one.
        m_cursorsPlayer = session.playerGuid();
        m_eventCursor = play.events().end();
        m_socialCursor = play.social().events().end();
        reset();
        m_lastMs = nowMs;
    }
    readEvents(session, nowMs);
    readSocial(session);
    watchState(session, nowMs);
    m_stack.update(nowMs - m_lastMs);
    m_lastMs = nowMs;
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        answer(session, o);
    }
}

// --- What their answers do ---------------------------------------------------------------------------------

void PopupScreens::answer(GameSession& session, const PopupOutcome& o) {
    game::Gameplay& play = session.play();
    const bool accepted = o.end == PopupEnd::Accept;
    // OnCancel for every reason but "override" (a newer popup of the same question took its place) and
    // Hidden.
    const bool cancelled =
        o.end == PopupEnd::Cancel || o.end == PopupEnd::Timeout || o.end == PopupEnd::Refused;
    if (m_trace) {
        VD_LOG_INFO("Popup %s %s (data %llx)", kindName(o.kind), endName(o.end),
                    static_cast<unsigned long long>(o.data));
    }
    if (o.shown && !o.stays) {
        m_uiSound(UiSound::PanelClose); // StaticPopup_OnHide
    }
    switch (o.kind) {
    case PopupKind::Death:
        if (accepted) {
            play.releaseSpirit();               // RepopMe
        } else if (o.end == PopupEnd::Cancel) { // the second button: UseSoulstone (or RepopMe without one)
            if (play.death().selfResSpell != 0) {
                play.selfResurrect();
            } else {
                play.releaseSpirit();
            }
        }
        break;
    case PopupKind::Resurrect:
    case PopupKind::ResurrectNoSickness:
    case PopupKind::ResurrectNoTimer:
        if (accepted) {
            play.answerResurrect(true);
        } else if (cancelled && play.death().resurrector == o.data) {
            play.answerResurrect(false); // DeclineResurrect, then DEATH again while the body lies there
            showDeath(session, session.lastUpdateMs());
        }
        break;
    case PopupKind::RecoverCorpse:
        if (accepted) {
            play.reclaimCorpse(); // RetrieveCorpse (the popup stays until the player is alive)
        }
        break;
    case PopupKind::ConfirmResetInstances:
        if (accepted) {
            play.travel().resetInstances(); // ResetInstances
        }
        break;
    case PopupKind::ConfirmLeaveQueue: // phase 16 (alerts)
        if (accepted) {
            play.meetingStone().leave(m_lastMs); // CancelMeetingStoneRequest
        }
        break;
    case PopupKind::ConfirmBattlefieldEntry: // phase 17 (queue): no OnCancel (HIDE leaves the queue as it is)
        if (accepted) {
            play.battlegrounds().acceptPort(static_cast<u32>(o.data), true); // AcceptBattlefieldPort(data, 1)
        }
        break;
    case PopupKind::RecoverCorpseInstance:
    case PopupKind::InstanceBoot: break; // no buttons
    case PopupKind::AreaSpiritHeal: // phase 17 (honor): Cancel is its OnAccept (CancelAreaSpiritHeal)
        if (accepted) {
            play.areaSpiritHealer().cancel();
        }
        break;
    case PopupKind::XpLoss:
    case PopupKind::XpLossNoSickness:
        if (accepted) {
            play.acceptSpiritHealer(); // AcceptXPLoss
        }
        break;
    case PopupKind::PartyInvite:
        if (accepted) {
            play.social().acceptGroupInvite();
        } else if (cancelled) {
            play.social().declineGroupInvite(); // OnCancel, and OnHide while not accepted
        }
        break;
    case PopupKind::GuildInvite:
        if (accepted) {
            play.social().acceptGuildInvite();
        } else if (cancelled) {
            play.social().declineGuildInvite();
        }
        break;
    case PopupKind::DuelRequested:
        if (play.social().duel().phase == game::DuelPhase::Challenged && (accepted || cancelled)) {
            if (accepted) {
                play.social().acceptDuel();
            } else {
                play.social().declineDuel(); // CancelDuel
            }
        }
        break;
    case PopupKind::Trade: {
        game::Trade& trade = play.exchange().trade();
        if (trade.phase() == game::TradePhase::Requested && (accepted || cancelled)) {
            if (accepted) {
                trade.acceptRequest(); // BeginTrade
            } else {
                trade.declineRequest(); // CancelTrade
            }
        }
        break;
    }
    case PopupKind::TradeWaiting:
        if (accepted && play.exchange().trade().phase() == game::TradePhase::Proposed) {
            play.exchange().trade().cancel();
        }
        break;
    case PopupKind::QuestAccept:
        if (accepted) {
            play.confirmSharedQuest(static_cast<u32>(o.data)); // ConfirmAcceptQuest
        }
        break;
    case PopupKind::AbandonQuest:
    case PopupKind::AbandonQuestWithItems: // phase 18 (p3)
        if (accepted) {
            play.abandonQuest(static_cast<u32>(o.data));
            m_uiSound(UiSound::QuestAbandon); // igQuestLogAbandonQuest
        }
        break;
    case PopupKind::NameChat: // phase 18 (p3): FCF_SetWindowName, or FCF_OpenNewWindow without a window
        if (accepted && m_chatNamer) {
            m_chatNamer(m_chatNamerContext, static_cast<u32>(o.data), {m_editText.data(), m_editLength});
        }
        break;
    case PopupKind::DeleteMail: // phase 18 (p3): DeleteInboxItem(InboxFrame.openMailID)
    case PopupKind::DeleteMoney:
        if (accepted) {
            play.exchange().mail().remove(static_cast<u32>(o.data));
        }
        break;
    case PopupKind::CodConfirmation: // retouches (windows): TakeInboxItem(InboxFrame.openMailID)
        if (accepted) {
            play.exchange().mail().takeItem(static_cast<u32>(o.data));
        }
        break;
    case PopupKind::CodAlert: break;
    case PopupKind::DeleteItem:
    case PopupKind::DeleteGoodItem: {
        const game::ItemPos pos = unpackPos(o.data);
        const game::ItemInstance* item = play.inventory().at(pos);
        if (accepted && item && item->guid == o.data2) {
            play.inventory().destroy(pos); // DeleteCursorItem
        }
        break;
    }
    case PopupKind::ConfirmBinder:
        if (play.travel().binderAsking() != 0 && (accepted || o.end != PopupEnd::Override)) {
            play.travel().answerBinder(accepted); // ConfirmBinder; otherwise the question is forgotten
        }
        break;
    case PopupKind::ConfirmTalentWipe:
        if (play.abilities().talentWipeTrainer() != 0 && (accepted || o.end != PopupEnd::Override)) {
            play.abilities().answerTalentWipe(accepted); // ConfirmTalentWipe
        }
        break;
    case PopupKind::ConfirmSummon:
        if (accepted) {
            play.summon().accept(session.lastUpdateMs()); // ConfirmSummon
        } else if (cancelled) {
            play.summon().decline();
        }
        break;
    case PopupKind::Camp:
        // The button is CANCEL (CancelLogout); OnHide cancels too while time was left.
        if (accepted || o.timeLeftMs > 0) {
            play.logout().cancel();
        }
        break;
    case PopupKind::Quit:
        if (accepted) {
            session.forceQuit(); // QUIT_NOW: ForceQuit
        } else if (o.timeLeftMs > 0) {
            play.logout().cancel();
        }
        break;
    case PopupKind::LootBind:
    case PopupKind::EquipBind:
    case PopupKind::AutoEquipBind:
    case PopupKind::UseBind: {
        const PendingBind bind = m_bind;
        if (o.data == m_bindSerial) {
            m_bind = PendingBind{}; // CancelPendingEquip, unless OKAY does it now
        }
        if (!accepted || o.data != m_bindSerial) {
            break;
        }
        switch (bind.ask) {
        case game::BindAsk::Loot: // LootSlot, now confirmed
            if (play.loot().open() && play.loot().guid() == bind.lootGuid) {
                play.lootItem(bind.lootSlot);
            }
            break;
        case game::BindAsk::Equip: // EquipPendingItem: the drop that was held back
            game::dropItem(play, bind.from, bind.to);
            break;
        case game::BindAsk::AutoEquip:
            if (bind.to.kind != game::ItemPlace::Kind::None) {
                game::dropItem(play, bind.from, bind.to);
            } else {
                game::doItemAction(play, bind.from, game::ItemAction::Equip);
            }
            break;
        case game::BindAsk::Use: // ConfirmBindOnUse
            if (bind.itemEntry != 0) {
                play.useItem(bind.itemEntry);
            } else {
                game::doItemAction(play, bind.from, game::ItemAction::Use);
            }
            break;
        case game::BindAsk::None: break;
        }
        break;
    }
    case PopupKind::AbandonPet:
        if (accepted) {
            play.abilities().abandonPet(); // PetAbandon
        }
        break;
    case PopupKind::ConfirmBuyBankSlot:
        if (accepted && play.inventory().banker() == o.data) {
            play.inventory().buyBankSlot(); // PurchaseSlot
        }
        break;
    case PopupKind::BindEnchant: // phase 13 (spells): BindEnchant / ReplaceEnchant / ReplaceTradeEnchant
    case PopupKind::ReplaceEnchant:
    case PopupKind::TradeReplaceEnchant:
        if (accepted) {
            play.confirmEnchant();
        } else if (cancelled) {
            play.declineEnchant(); // the spell still waits for an item
        }
        break;
    // Phase 13 (social).
    case PopupKind::ConfirmGuildDisband:
        if (accepted) {
            play.commands().guildDisband(); // GuildDisband
        }
        break;
    case PopupKind::ConfirmGuildPromote:
        if (accepted) {
            play.commands().guildSetLeader(o.name.data()); // GuildSetLeaderByName(data)
        }
        break;
    case PopupKind::ConfirmGuildLeave:
        if (accepted) {
            play.social().leaveGuild(); // GuildLeave
        }
        break;
    case PopupKind::RenamePet:
        // OnAccept / EditBoxOnEnterPressed: PETRENAMECONFIRM with the name typed.
        if (accepted && m_editLength > 0) {
            if (Popup* p = show(session, PopupKind::PetRenameConfirm, o.data)) {
                p->setName({m_editText.data(), m_editLength});
            }
        }
        break;
    case PopupKind::PetRenameConfirm:
        if (accepted) {
            play.commands().renamePet(o.data, o.name.data()); // PetRename(this:GetParent().data)
        }
        break;
    case PopupKind::ConfirmPetUnlearn: // phase 15 (pets): ConfirmPetUnlearn, or the offer forgotten
        play.stable().answerUnlearn(accepted);
        break;
    // Phase 15 (items).
    case PopupKind::ConfirmLootRoll: // ConfirmLootRoll(id, rollType)
        if (accepted) {
            play.exchange().rolls().roll(o.data, static_cast<u32>(o.data2 & 0xFFFFFFFFu), static_cast<u8>(o.data2 >> 32));
        }
        break;
    case PopupKind::ConfirmLootDistribution: // GiveMasterLoot(LootFrame.selectedSlot, data)
        if (accepted && play.loot().open() && (play.loot().guid() & 0x00FFFFFFFFFFFFFFull) == (o.data2 >> 8)) {
            play.exchange().rolls().giveMasterLoot(play.loot().guid(), static_cast<u8>(o.data2 & 0xFF), o.data);
        }
        break;
    case PopupKind::UnlearnSkill: // phase 15 (character)
        if (accepted) {
            play.character().unlearnSkill(static_cast<u32>(o.data)); // AbandonSkill
        }
        break;
    // Phase 15 (guild): the typed text (m_editText) for the edit boxes.
    case PopupKind::AddGuildMember:
        if (accepted && m_editLength > 0) {
            play.social().guildInvite({m_editText.data(), m_editLength}); // GuildInviteByName
        }
        break;
    case PopupKind::AddRaidMember: // phase 16 (raidframe)
        if (accepted && m_editLength > 0) {
            play.social().inviteToGroup({m_editText.data(), m_editLength}); // InviteByName
        }
        break;
    case PopupKind::AddFriend: // retouches (friends frame)
        if (accepted && m_editLength > 0) {
            play.social().addFriend({m_editText.data(), m_editLength}); // AddFriend
        }
        break;
    case PopupKind::AddIgnore:
        if (accepted && m_editLength > 0) {
            play.social().addIgnore({m_editText.data(), m_editLength}); // AddIgnore
        }
        break;
    case PopupKind::RemoveGuildMember:
        if (accepted) {
            play.social().guildRemove(o.name.data()); // GuildUninviteByName(GuildFrame.selectedName)
        }
        break;
    case PopupKind::AddGuildRank:
        if (accepted && m_editLength > 0) {
            play.guildControl().addRank({m_editText.data(), m_editLength}); // GuildControlAddRank
        }
        break;
    case PopupKind::SetGuildMotd:
        if (accepted) {
            play.social().setGuildMotd({m_editText.data(), m_editLength}); // GuildSetMOTD (empty clears it)
        }
        break;
    case PopupKind::SetGuildPlayerNote:
        if (accepted) {
            play.guildControl().setPublicNote(o.name.data(), {m_editText.data(), m_editLength});
        }
        break;
    case PopupKind::SetGuildOfficerNote:
        if (accepted) {
            play.guildControl().setOfficerNote(o.name.data(), {m_editText.data(), m_editLength});
        }
        break;
    case PopupKind::RenameGuild:
        if (accepted && m_editLength > 0) {
            play.petitions().rename({m_editText.data(), m_editLength}, session.lastUpdateMs()); // RenamePetition
        }
        break;
    // Phase 15 (help).
    case PopupKind::HelpTicket:
        // OnAccept: the ticket page (or HELP_TICKET_QUEUE_DISABLED while the queue is down); OnCancel ("Abandon My
        // Ticket"): HELP_TICKET_ABANDON_CONFIRM.
        if (accepted) {
            if (play.help().queueActive()) {
                if (m_helpOpener) {
                    m_helpOpener(m_helpContext, session);
                }
            } else {
                show(session, PopupKind::HelpTicketQueueDisabled);
            }
        } else if (cancelled) {
            show(session, PopupKind::HelpTicketAbandonConfirm);
        }
        break;
    case PopupKind::HelpTicketAbandonConfirm:
        if (accepted) {
            play.help().remove(); // DeleteGMTicket
        }
        break;
    case PopupKind::HelpTicketQueueDisabled:
    case PopupKind::DuelOutOfBounds:
    case PopupKind::Count: break;
    }
}

// --- Drawing -----------------------------------------------------------------------------------------------

std::string_view PopupScreens::text(GameSession& session, const Popup& p, std::span<char> out, u32 nowMs) {
    (void)nowMs;
    const PopupInfo& info = popupInfo(p.kind);
    game::Gameplay& play = session.play();
    const Countdown left = countdown(p.timeLeftMs);
    const Countdown delay = countdown(p.startDelayMs);
    switch (p.kind) {
    case PopupKind::Death:
        return p.data2 != 0 && p.timeLeftMs > 0 ? text::format(out, "DEATH_RELEASE_TIMER", left.n, left.unit)
                                                : text::format(out, "DEATH_RELEASE_NOTIMER");
    case PopupKind::Resurrect:
    case PopupKind::ResurrectNoSickness:
    case PopupKind::ResurrectNoTimer:
        return p.startDelayMs > 0 ? text::format(out, info.delayText, p.nameView(), delay.n, delay.unit)
                                  : text::format(out, info.text, p.nameView());
    case PopupKind::RecoverCorpse:
        return p.startDelayMs > 0 ? text::format(out, info.delayText, delay.n, delay.unit)
                                  : text::format(out, info.text);
    case PopupKind::XpLoss:
    case PopupKind::XpLossNoSickness:
        // CONFIRM_XP_LOSS_AGAIN_NO_SICKNESS is set as it is, not formatted (its "25%" has no second '%').
        if (p.kind == PopupKind::XpLossNoSickness && p.again) {
            return text::get(info.againText);
        }
        return text::format(out, p.again ? info.againText : info.text, p.nameView());
    case PopupKind::PartyInvite: return text::format(out, info.text, p.nameView());
    case PopupKind::GuildInvite: return text::format(out, info.text, p.nameView(), p.name2View());
    case PopupKind::DuelRequested:
    case PopupKind::Trade:
    case PopupKind::TradeWaiting: return text::format(out, info.text, playerName(session, p.data));
    case PopupKind::DuelOutOfBounds:
    case PopupKind::Camp:
    case PopupKind::Quit: return text::format(out, info.text, left.n, left.unit);
    case PopupKind::QuestAccept: {
        const game::QuestInfo* quest = play.quest(static_cast<u32>(p.data));
        return text::format(out, info.text, playerName(session, p.data2),
                            quest ? std::string_view(quest->title) : std::string_view("..."));
    }
    case PopupKind::AbandonQuest: {
        const game::QuestInfo* quest = play.quest(static_cast<u32>(p.data));
        return text::format(out, info.text, quest ? std::string_view(quest->title) : std::string_view("..."));
    }
    case PopupKind::AbandonQuestWithItems: { // phase 18 (p3): GetAbandonQuestName(), items
        const game::QuestInfo* quest = play.quest(static_cast<u32>(p.data));
        return text::format(out, info.text, quest ? std::string_view(quest->title) : std::string_view("..."),
                            std::string_view(m_abandonItems));
    }
    case PopupKind::DeleteMail: return text::format(out, info.text, p.nameView()); // phase 18 (p3)
    case PopupKind::DeleteItem:
    case PopupKind::DeleteGoodItem: {
        const game::ItemInstance* item = play.inventory().at(unpackPos(p.data));
        const net::ItemQueryInfo* known = item ? session.item(item->entry) : nullptr;
        const std::string_view name = known ? std::string_view(known->name) : std::string_view("...");
        if (p.kind == PopupKind::DeleteItem && item && item->count > 1) {
            return text::format(out, "VD_INV_DESTROY_STACK", name,
                                item->count); // ours: a stack says how many
        }
        return text::format(out, info.text, name);
    }
    case PopupKind::ConfirmBinder: {
        // The inn's place: the subzone the player stands in.
        const game::ZoneState& zone = play.travel().zone();
        const std::string_view place = !zone.subzoneName.empty() ? zone.subzoneName
                                       : !zone.zoneName.empty()  ? zone.zoneName
                                                                 : text::get("VD_TRAVEL_THIS_INN");
        return text::format(out, info.text, place);
    }
    case PopupKind::ReplaceEnchant: // phase 13 (spells): the enchantments' names (SpellItemEnchantment.dbc)
    case PopupKind::TradeReplaceEnchant: {
        const std::optional<game::EnchantmentInfo> old =
            m_data ? m_data->enchantment(static_cast<u32>(p.data)) : std::nullopt;
        const std::optional<game::EnchantmentInfo> next =
            m_data ? m_data->enchantment(static_cast<u32>(p.data >> 32)) : std::nullopt;
        return text::format(out, info.text, old ? old->name : std::string_view("..."),
                            next ? next->name : std::string_view("..."));
    }
    case PopupKind::ConfirmSummon: {
        const std::string_view area =
            m_data ? m_data->areaName(static_cast<u32>(p.data2)) : std::string_view{};
        return text::format(out, info.text, playerName(session, p.data),
                            area.empty() ? std::string_view("...") : area, left.n, left.unit);
    }
    case PopupKind::InstanceBoot: {
        // StaticPopup_OnUpdate: format(text, GetBindLocation(), time, unit), where the hearthstone leads.
        const game::BindPoint& bind = play.travel().bindPoint();
        const std::string_view where =
            m_data && bind.known ? m_data->areaName(bind.areaId) : std::string_view{};
        return text::format(out, info.text, where.empty() ? std::string_view("...") : where, left.n, left.unit);
    }
    case PopupKind::ConfirmGuildPromote:
    case PopupKind::ConfirmGuildLeave:
    case PopupKind::PetRenameConfirm:
    case PopupKind::RemoveGuildMember: return text::format(out, info.text, p.nameView()); // phase 13, 15 (guild)
    case PopupKind::ConfirmLootDistribution: // phase 15 (items): "You wish to assign %s to %s."
        return text::format(out, info.text, p.nameView(), p.name2View());
    case PopupKind::UnlearnSkill: return text::format(out, info.text, p.nameView());     // phase 15 (character)
    case PopupKind::AreaSpiritHeal: // phase 17 (honor): " " until the countdown fills it (StaticPopup_Show)
        return p.timeLeftMs > 0 ? text::format(out, info.text, left.n, left.unit) : std::string_view(" ");
    case PopupKind::ConfirmBattlefieldEntry: return text::format(out, info.text, p.nameView()); // phase 17
    default: return text::format(out, info.text);
    }
}

std::string_view PopupScreens::button2Text(GameSession& session, const Popup& p) {
    const PopupInfo& info = popupInfo(p.kind);
    if (info.button2.name.empty() || !p.button2Shown) {
        return {};
    }
    if (p.kind == PopupKind::Death) {
        // HasSoulstone(): the self-resurrection's own name on the button (Soulstone Resurrection,
        // Reincarnation...).
        const u32 spellId = session.play().death().selfResSpell;
        const std::optional<game::SpellInfo> spell =
            m_data && spellId ? m_data->spell(spellId) : std::nullopt;
        if (spell && !spell->name.empty()) {
            return spell->name;
        }
    }
    return text::get(info.button2);
}

void PopupScreens::draw(GameSession& session, DialogContext& c, ui::TextInput& input, u32 nowMs) {
    VD_PROFILE_ZONE("Popups::screens");
    // DELETE_GOOD_ITEM's edit box: what the keyboard typed, and Enter (EditBoxOnEnterPressed).
    i32 submitSlot = -1;
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        Popup& p = m_stack.slot(i);
        if (!hasEdit(p.kind)) {
            continue;
        }
        if (input.field() == editId(i)) {
            const std::string_view typed = input.text();
            m_editLength = static_cast<u32>(std::min(typed.size(), m_editText.size() - 1));
            std::memcpy(m_editText.data(), typed.data(), m_editLength);
            m_editText[m_editLength] = '\0';
            if (input.submitted()) {
                submitSlot = static_cast<i32>(i);
            }
        }
        if (p.kind != PopupKind::DeleteGoodItem) {
            continue; // RENAME_PET, the guild's: Accept always (an empty name does nothing)
        }
        // EditBoxOnTextChanged: Yes only once DELETE_ITEM_CONFIRM_STRING is typed (any case).
        const std::string_view confirm = text::get("DELETE_ITEM_CONFIRM_STRING");
        bool same = m_editLength == confirm.size();
        for (u32 k = 0; same && k < m_editLength; ++k) {
            same = std::toupper(static_cast<unsigned char>(m_editText[k])) ==
                   std::toupper(static_cast<unsigned char>(confirm[k]));
        }
        p.button1Enabled = same;
    }
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        const Popup& p = m_stack.slots()[i];
        PopupView& v = m_views[i];
        v = PopupView{};
        if (!p.shown()) {
            continue;
        }
        const PopupInfo& info = popupInfo(p.kind);
        v.text = text(session, p, m_texts[i], nowMs);
        v.button1 = info.button1.name.empty() ? std::string_view{} : text::get(info.button1);
        v.button2 = button2Text(session, p);
        v.button1Enabled = PopupStack::button1Ready(p);
        v.alert = info.has(PopupInfo::kShowAlert);
        v.money = info.has(PopupInfo::kMoneyFrame);
        v.copper = p.money;
        v.editBox = info.has(PopupInfo::kEditBox);
        v.editText = {m_editText.data(), m_editLength};
        v.editing = input.field() == editId(i);
    }
    const PopupTaps taps = popupStack(c, m_views, m_heights);
    i32 focus = taps.editSlot;
    for (u32 i = 0; i < PopupStack::kSlots && m_focusEdit && focus < 0; ++i) {
        focus = hasEdit(m_stack.slots()[i].kind) ? static_cast<i32>(i) : -1;
    }
    m_focusEdit = false;
    if (focus >= 0 && input.field() != editId(static_cast<u32>(focus))) {
        input.begin(editId(static_cast<u32>(focus)), {m_editText.data(), m_editLength},
                    editLetters(m_stack.slots()[static_cast<u32>(focus)].kind)); // maxLetters
    }
    if (taps.slot >= 0) {
        m_stack.click(static_cast<u32>(taps.slot), taps.button);
    } else if (submitSlot >= 0) {
        m_stack.click(static_cast<u32>(submitSlot), 1); // a disabled Yes does nothing
    }
    // A closed DELETE_GOOD_ITEM lets go of the keyboard.
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        if (input.field() == editId(i) && !hasEdit(m_stack.slots()[i].kind)) {
            input.end();
        }
    }
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        answer(session, o);
    }
}

} // namespace vd
