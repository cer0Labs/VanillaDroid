#include "app/PopupStack.h"

namespace vd {
namespace {

using F = PopupInfo;

constexpr u32 kSecond = 1000;

// StaticPopupDialogs (StaticPopup.lua), in PopupKind order. The kinds whose OnShow sets dialog.timeleft
// (DEATH: GetReleaseTimeRemaining, RESURRECT*: the corpse delay + 60, CONFIRM_SUMMON:
// GetSummonConfirmTimeLeft) get it from the caller; their table timeout is 0 as in the original, except
// RESURRECT's 60.
const PopupInfo kInfo[] = {
    // DEATH
    {"DEATH_RELEASE_TIMER", "", "", "DEATH_RELEASE", "USE_SOULSTONE", 0,
     F::kWhileDead | F::kNotClosableByLogout | F::kCountdown, PopupKind::RecoverCorpse},
    // RESURRECT, RESURRECT_NO_SICKNESS, RESURRECT_NO_TIMER
    {"RESURRECT_REQUEST", "RESURRECT_REQUEST_TIMER", "", "ACCEPT", "DECLINE", 60 * kSecond,
     F::kWhileDead | F::kNotClosableByLogout | F::kHideOnEscape, PopupKind::Death},
    {"RESURRECT_REQUEST_NO_SICKNESS", "RESURRECT_REQUEST_NO_SICKNESS_TIMER", "", "ACCEPT", "DECLINE",
     60 * kSecond, F::kWhileDead | F::kNotClosableByLogout | F::kHideOnEscape, PopupKind::Death},
    {"RESURRECT_REQUEST_NO_SICKNESS", "", "", "ACCEPT", "DECLINE", 60 * kSecond,
     F::kWhileDead | F::kNotClosableByLogout | F::kHideOnEscape, PopupKind::Death},
    // RECOVER_CORPSE
    {"RECOVER_CORPSE", "RECOVER_CORPSE_TIMER", "", "ACCEPT", "", 0,
     F::kWhileDead | F::kNotClosableByLogout | F::kKeepOnAccept},
    // XP_LOSS, XP_LOSS_NO_SICKNESS
    {"CONFIRM_XP_LOSS", "", "CONFIRM_XP_LOSS_AGAIN", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kShowAlert | F::kHideOnEscape},
    {"CONFIRM_XP_LOSS_NO_SICKNESS", "", "CONFIRM_XP_LOSS_AGAIN_NO_SICKNESS", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kShowAlert | F::kHideOnEscape},
    // PARTY_INVITE, GUILD_INVITE, DUEL_REQUESTED, DUEL_OUTOFBOUNDS
    {"INVITATION", "", "", "ACCEPT", "DECLINE", 60 * kSecond, F::kWhileDead | F::kHideOnEscape},
    {"GUILD_INVITATION", "", "", "ACCEPT", "DECLINE", 60 * kSecond, F::kWhileDead | F::kHideOnEscape},
    {"DUEL_REQUESTED", "", "", "ACCEPT", "DECLINE", 60 * kSecond, F::kHideOnEscape},
    {"DUEL_OUTOFBOUNDS_TIMER", "", "", "", "", 10 * kSecond, F::kCountdown},
    // TRADE, and ours while the player's own request waits
    {"TRADE_WITH_QUESTION", "", "", "YES", "NO", 60 * kSecond, F::kHideOnEscape},
    {"VD_EXCHANGE_TRADE_WAITING", "", "", "CANCEL", "", 0, F::kHideOnEscape},
    // QUEST_ACCEPT, ABANDON_QUEST
    {"QUEST_ACCEPT", "", "", "YES", "NO", 0, F::kExclusive | F::kHideOnEscape},
    {"ABANDON_QUEST_CONFIRM", "", "", "YES", "NO", 0, F::kWhileDead | F::kExclusive | F::kHideOnEscape},
    // DELETE_ITEM, DELETE_GOOD_ITEM
    {"DELETE_ITEM", "", "", "YES", "NO", 0, F::kWhileDead | F::kExclusive | F::kShowAlert | F::kHideOnEscape},
    {"DELETE_GOOD_ITEM", "", "", "YES", "NO", 0,
     F::kWhileDead | F::kExclusive | F::kShowAlert | F::kHideOnEscape | F::kEditBox},
    // CONFIRM_BINDER, CONFIRM_TALENT_WIPE, CONFIRM_SUMMON
    {"CONFIRM_BINDER", "", "", "ACCEPT", "CANCEL", 0, F::kHideOnEscape},
    {"CONFIRM_TALENT_WIPE", "", "", "ACCEPT", "CANCEL", 0, F::kMoneyFrame | F::kHideOnEscape},
    {"CONFIRM_SUMMON", "", "", "ACCEPT", "CANCEL", 0,
     F::kNotClosableByLogout | F::kHideOnEscape | F::kCountdown},
    // CAMP (its button2 CAMP_NOW is commented out in 1.12), QUIT
    {"CAMP_TIMER", "", "", "CANCEL", "", 20 * kSecond, F::kWhileDead | F::kHideOnEscape | F::kCountdown},
    {"QUIT_TIMER", "", "", "QUIT_NOW", "CANCEL", 20 * kSecond,
     F::kWhileDead | F::kHideOnEscape | F::kCountdown},
    // LOOT_BIND, EQUIP_BIND, AUTOEQUIP_BIND, USE_BIND
    {"LOOT_NO_DROP", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kHideOnEscape},
    {"EQUIP_NO_DROP", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kWhileDead | F::kHideOnEscape},
    {"EQUIP_NO_DROP", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kWhileDead | F::kHideOnEscape},
    {"USE_NO_DROP", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kHideOnEscape},
    // ABANDON_PET
    {"ABANDON_PET", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kHideOnEscape},
    // CONFIRM_BUY_BANK_SLOT
    {"CONFIRM_BUY_BANK_SLOT", "", "", "YES", "NO", 0, F::kMoneyFrame | F::kHideOnEscape},
    // CONFIRM_RESET_INSTANCES, RECOVER_CORPSE_INSTANCE, INSTANCE_BOOT (its OnShow sets the time left:
    // GetInstanceBootTimeRemaining; at 0 StaticPopup_OnUpdate hides it)
    {"CONFIRM_RESET_INSTANCES", "", "", "YES", "NO", 0, F::kWhileDead | F::kHideOnEscape},
    {"RECOVER_CORPSE_INSTANCE", "", "", "", "", 0, F::kWhileDead | F::kNotClosableByLogout},
    {"INSTANCE_BOOT_TIMER", "", "", "", "", 0, F::kWhileDead | F::kNotClosableByLogout | F::kCountdown},
    // Phase 13 (spells): BIND_ENCHANT, REPLACE_ENCHANT, TRADE_REPLACE_ENCHANT
    {"BIND_ENCHANT", "", "", "OKAY", "CANCEL", 0, F::kExclusive | F::kShowAlert | F::kHideOnEscape},
    {"REPLACE_ENCHANT", "", "", "YES", "NO", 0, F::kExclusive | F::kShowAlert | F::kHideOnEscape},
    {"REPLACE_ENCHANT", "", "", "YES", "NO", 0, F::kExclusive | F::kShowAlert | F::kHideOnEscape},
    // Phase 13 (social): CONFIRM_GUILD_DISBAND, CONFIRM_GUILD_PROMOTE, CONFIRM_GUILD_LEAVE, RENAME_PET,
    // PETRENAMECONFIRM
    {"CONFIRM_GUILD_DISBAND", "", "", "YES", "NO", 0, F::kWhileDead | F::kHideOnEscape},
    {"CONFIRM_GUILD_PROMOTE", "", "", "ACCEPT", "CANCEL", 0, F::kWhileDead | F::kHideOnEscape},
    {"CONFIRM_GUILD_LEAVE", "", "", "ACCEPT", "CANCEL", 0, F::kWhileDead | F::kHideOnEscape},
    {"PET_RENAME_LABEL", "", "", "ACCEPT", "CANCEL", 0, F::kExclusive | F::kHideOnEscape | F::kEditBox},
    {"PET_RENAME_CONFIRMATION", "", "", "YES", "NO", 0, F::kHideOnEscape},
    // Phase 15 (pets): CONFIRM_PET_UNLEARN (hasMoneyFrame; its OnUpdate: CheckPetUntrainerDist)
    {"CONFIRM_PET_UNLEARN", "", "", "ACCEPT", "CANCEL", 0, F::kMoneyFrame | F::kHideOnEscape},
    // Phase 15 (items): CONFIRM_LOOT_ROLL, CONFIRM_LOOT_DISTRIBUTION
    {"LOOT_NO_DROP", "", "", "OKAY", "CANCEL", 0, F::kWhileDead | F::kExclusive | F::kHideOnEscape},
    {"CONFIRM_LOOT_DISTRIBUTION", "", "", "YES", "NO", 0, F::kHideOnEscape},
    // Phase 15 (character): UNLEARN_SKILL
    {"UNLEARN_SKILL", "", "", "UNLEARN", "CANCEL", 60 * kSecond,
     F::kExclusive | F::kWhileDead | F::kShowAlert | F::kHideOnEscape},
    // Phase 15 (guild): ADD_GUILDMEMBER, REMOVE_GUILDMEMBER, ADD_GUILDRANK, SET_GUILDMOTD, SET_GUILDPLAYERNOTE,
    // SET_GUILDOFFICERNOTE, RENAME_GUILD, HELP_TICKET, HELP_TICKET_ABANDON_CONFIRM, HELP_TICKET_QUEUE_DISABLED
    {"ADD_GUILDMEMBER_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"REMOVE_GUILDMEMBER_LABEL", "", "", "YES", "NO", 0, F::kExclusive | F::kWhileDead | F::kHideOnEscape},
    {"ADD_GUILDRANK_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"SET_GUILDMOTD_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"SET_GUILDPLAYERNOTE_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"SET_GUILDOFFICERNOTE_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"RENAME_GUILD_LABEL", "", "", "ACCEPT", "CANCEL", 0,
     F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"HELP_TICKET_EDIT_ABANDON", "", "", "HELP_TICKET_EDIT", "HELP_TICKET_ABANDON", 0, F::kWhileDead},
    {"HELP_TICKET_ABANDON_CONFIRM", "", "", "YES", "NO", 0, F::kWhileDead | F::kHideOnEscape},
    {"HELP_TICKET_QUEUE_DISABLED", "", "", "OKAY", "", 0, F::kShowAlert},
    // Phase 16 (raidframe): ADD_RAIDMEMBER (no whileDead)
    {"ADD_RAIDMEMBER_LABEL", "", "", "ACCEPT", "CANCEL", 0, F::kExclusive | F::kHideOnEscape | F::kEditBox},
    // Phase 16 (alerts): CONFIRM_LEAVE_QUEUE
    {"CONFIRM_LEAVE_QUEUE", "", "", "ACCEPT", "CANCEL", 0, F::kHideOnEscape},
    // Phase 17 (honor): AREA_SPIRIT_HEAL (1.12.1's second definition: button1 CANCEL, OnShow
    // AcceptAreaSpiritHeal; its OnShow sets the time left: GetAreaSpiritHealerTime; at 0 StaticPopup_OnUpdate
    // hides it)
    {"AREA_SPIRIT_HEAL", "", "", "CANCEL", "", 0,
     F::kWhileDead | F::kNotClosableByLogout | F::kHideOnEscape | F::kCountdown},
    // Phase 17 (queue): CONFIRM_BATTLEFIELD_ENTRY (multiple = 1: one per queue; HIDE only hides it)
    {"CONFIRM_BATTLEFIELD_ENTRY", "", "", "ENTER_BATTLE", "HIDE", 0,
     F::kWhileDead | F::kHideOnEscape | F::kMultiple},
    // Phase 18 (p3): ABANDON_QUEST_WITH_ITEMS, DELETE_MAIL, DELETE_MONEY
    {"ABANDON_QUEST_CONFIRM_WITH_ITEMS", "", "", "YES", "NO", 0, F::kWhileDead | F::kExclusive | F::kHideOnEscape},
    {"DELETE_MAIL_CONFIRMATION", "", "", "ACCEPT", "CANCEL", 0, F::kShowAlert | F::kHideOnEscape},
    {"DELETE_MONEY_CONFIRMATION", "", "", "ACCEPT", "CANCEL", 0, F::kMoneyFrame | F::kShowAlert | F::kHideOnEscape},
    {"NAME_CHAT_WINDOW", "", "", "ACCEPT", "CANCEL", 0, F::kWhileDead | F::kEditBox}, // NAME_CHAT (maxLetters 31)
    // Retouches (windows): COD_ALERT, COD_CONFIRMATION
    {"COD_INSUFFICIENT_MONEY", "", "", "CLOSE", "", 0, F::kHideOnEscape},
    {"COD_CONFIRMATION", "", "", "ACCEPT", "CANCEL", 0, F::kMoneyFrame | F::kHideOnEscape},
    // Retouches (friends frame): ADD_FRIEND, ADD_IGNORE
    {"ADD_FRIEND_LABEL", "", "", "ACCEPT", "CANCEL", 0, F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
    {"ADD_IGNORE_LABEL", "", "", "ACCEPT", "CANCEL", 0, F::kExclusive | F::kWhileDead | F::kHideOnEscape | F::kEditBox},
};
static_assert(std::size(kInfo) == static_cast<usize>(PopupKind::Count), "one entry per popup kind");

} // namespace

const PopupInfo& popupInfo(PopupKind kind) {
    return kInfo[kind < PopupKind::Count ? static_cast<u32>(kind) : 0];
}

void PopupStack::push(const PopupOutcome& outcome) {
    if (m_count == kOutcomes) { // nobody reads them: the oldest goes
        m_first = (m_first + 1) % kOutcomes;
        --m_count;
    }
    m_outcomes[(m_first + m_count) % kOutcomes] = outcome;
    ++m_count;
}

bool PopupStack::takeOutcome(PopupOutcome& out) {
    if (m_count == 0) {
        return false;
    }
    out = m_outcomes[m_first];
    m_first = (m_first + 1) % kOutcomes;
    --m_count;
    return true;
}

void PopupStack::close(u32 index, PopupEnd end) {
    Popup& p = m_slots[index];
    PopupOutcome o;
    o.kind = p.kind;
    o.end = end;
    o.data = p.data;
    o.data2 = p.data2;
    o.timeLeftMs = p.timeLeftMs;
    o.shown = true;
    o.name = p.name; // phase 13 (social)
    push(o);
    p = Popup{}; // the frame keeps its place; what it showed is gone
}

Popup* PopupStack::show(PopupKind kind, bool playerDead, u64 data) {
    if (kind >= PopupKind::Count) {
        return nullptr;
    }
    const PopupInfo& info = popupInfo(kind);
    const auto refuse = [&]() -> Popup* {
        PopupOutcome o;
        o.kind = kind;
        o.end = PopupEnd::Refused;
        o.data = data;
        push(o);
        return nullptr;
    };
    if (playerDead && !info.has(PopupInfo::kWhileDead)) {
        return refuse();
    }
    if (info.has(PopupInfo::kExclusive)) { // the first exclusive one shown goes
        for (u32 i = 0; i < kSlots; ++i) {
            if (m_slots[i].shown() && popupInfo(m_slots[i].kind).has(PopupInfo::kExclusive)) {
                close(i, PopupEnd::Override);
                break;
            }
        }
    }
    if (info.cancels != PopupKind::Count) {
        for (u32 i = 0; i < kSlots; ++i) {
            if (m_slots[i].kind == info.cancels) {
                close(i, PopupEnd::Override);
            }
        }
    }
    if (kind == PopupKind::Camp || kind == PopupKind::Quit) {
        for (u32 i = 0; i < kSlots; ++i) {
            if (m_slots[i].shown() && !popupInfo(m_slots[i].kind).has(PopupInfo::kNotClosableByLogout)) {
                close(i, PopupEnd::Override);
            }
        }
    }
    if (kind == PopupKind::Death) {
        for (u32 i = 0; i < kSlots; ++i) {
            if (m_slots[i].shown() && !popupInfo(m_slots[i].kind).has(PopupInfo::kWhileDead)) {
                close(i, PopupEnd::Override);
            }
        }
    }
    // The same kind already shown is replaced in its own slot (StaticPopup_FindVisible: with `multiple`, the
    // one about the same data); else the first free one.
    u32 index = kSlots;
    for (u32 i = 0; i < kSlots && index == kSlots; ++i) {
        if (m_slots[i].kind == kind && (!info.has(PopupInfo::kMultiple) || m_slots[i].data == data)) {
            close(i, PopupEnd::Override);
            index = i;
        }
    }
    for (u32 i = 0; i < kSlots && index == kSlots; ++i) {
        if (!m_slots[i].shown()) {
            index = i;
        }
    }
    if (index == kSlots) {
        return refuse();
    }
    Popup& p = m_slots[index];
    p = Popup{};
    p.kind = kind;
    p.data = data;
    p.timeLeftMs = static_cast<i32>(info.timeoutMs);
    return &p;
}

void PopupStack::hide(PopupKind kind) {
    for (u32 i = 0; i < kSlots; ++i) {
        if (m_slots[i].kind == kind) {
            close(i, PopupEnd::Hidden);
        }
    }
}

void PopupStack::hide(PopupKind kind, u64 data) {
    for (u32 i = 0; i < kSlots; ++i) {
        if (m_slots[i].kind == kind && m_slots[i].data == data) {
            close(i, PopupEnd::Hidden);
        }
    }
}

Popup* PopupStack::find(PopupKind kind) {
    for (Popup& p : m_slots) {
        if (p.kind == kind) {
            return &p;
        }
    }
    return nullptr;
}

const Popup* PopupStack::find(PopupKind kind) const {
    for (const Popup& p : m_slots) {
        if (p.kind == kind) {
            return &p;
        }
    }
    return nullptr;
}

bool PopupStack::any() const {
    return count() > 0;
}

u32 PopupStack::count() const {
    u32 n = 0;
    for (const Popup& p : m_slots) {
        n += p.shown() ? 1u : 0u;
    }
    return n;
}

void PopupStack::update(u32 elapsedMs) {
    const i32 elapsed = static_cast<i32>(elapsedMs);
    for (u32 i = 0; i < kSlots; ++i) {
        Popup& p = m_slots[i];
        if (!p.shown()) {
            continue;
        }
        if (p.timeLeftMs > 0) {
            p.timeLeftMs -= elapsed;
            if (p.timeLeftMs <= 0) {
                p.timeLeftMs = 0;
                close(i, PopupEnd::Timeout);
                continue;
            }
        }
        if (p.startDelayMs > 0) {
            p.startDelayMs = p.startDelayMs > elapsed ? p.startDelayMs - elapsed : 0;
        }
    }
}

void PopupStack::click(u32 index, u32 button) {
    if (index >= kSlots || !m_slots[index].shown()) {
        return;
    }
    Popup& p = m_slots[index];
    const PopupInfo& info = popupInfo(p.kind);
    if (button != 1) {
        close(index, PopupEnd::Cancel);
        return;
    }
    if (!button1Ready(p)) {
        return;
    }
    if (!info.againText.name.empty() && !p.again) {
        p.again = true; // XP_LOSS: "Remember, ..." first
        return;
    }
    if (info.has(PopupInfo::kKeepOnAccept)) {
        PopupOutcome o;
        o.kind = p.kind;
        o.end = PopupEnd::Accept;
        o.data = p.data;
        o.data2 = p.data2;
        o.timeLeftMs = p.timeLeftMs;
        o.shown = true;
        o.stays = true;
        push(o);
        return;
    }
    close(index, PopupEnd::Accept);
}

bool PopupStack::escape() {
    bool closed = false;
    for (u32 i = 0; i < kSlots; ++i) {
        if (m_slots[i].shown() && popupInfo(m_slots[i].kind).has(PopupInfo::kHideOnEscape)) {
            close(i, PopupEnd::Cancel);
            closed = true;
        }
    }
    return closed;
}

void PopupStack::clear() {
    for (u32 i = 0; i < kSlots; ++i) {
        if (m_slots[i].shown()) {
            close(i, PopupEnd::Hidden);
        }
    }
}

} // namespace vd
