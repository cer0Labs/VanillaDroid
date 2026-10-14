// Phase 18 (p3): the chat windows as WoW.exe 1.12.1 keeps them (game::ChatWindows: the defaults 0x4984C6, chat-cache.txt
// read by 0x498A60 and written by 0x499A80, FloatingChatFrame.lua's FCF_OpenNewWindow / FCF_Close), and the mailbox's
// InboxItemCanDelete (0x4AFE00) behind OpenMailFrame's one Delete / Return button.
#include "app/PopupStack.h"
#include "game/ChatWindows.h"
#include "game/Mail.h"

#include <doctest/doctest.h>

#include <string>

namespace {

using namespace vd;
using game::ChatWindows;

// What WoW.exe writes for a new character (its defaults), byte for byte.
std::string defaultFile() {
    std::string s = "VERSION 2\n\nADDEDVERSION 2\n\nOPTION_GUILD_RECRUITMENT_CHANNEL AUTO\n\nCHANNELS\nEND\n\n"
                    "ZONECHANNELS 0\n\nCOLORS\nEND\n\n";
    s += "WINDOW 1\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 1\nDOCKED 1\nSHOWN 1\n\nMESSAGES\nSYSTEM\nSAY\nYELL\nWHISPER\n"
         "PARTY\nGUILD\nCREATURE\nCHANNEL\nSKILL\nLOOT\nEND\n\nCHANNELS\nEND\n\nZONECHANNELS 0\n\nEND\n\n";
    s += "WINDOW 2\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 1\nDOCKED 2\nSHOWN 1\n\nMESSAGES\n";
    for (u32 g = 10; g < game::kChatGroups; ++g) {
        if (game::chatGroupInfo(g).byDefault) {
            s += game::chatGroupInfo(g).name;
            s += "\n";
        }
    }
    s += "END\n\nCHANNELS\nEND\n\nZONECHANNELS 0\n\nEND\n\n";
    for (u32 w = 3; w <= 10; ++w) {
        s += "WINDOW " + std::to_string(w) +
             "\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 0\nDOCKED 0\nSHOWN 0\n\nMESSAGES\nEND\n\nCHANNELS\nEND\n\n"
             "ZONECHANNELS 0\n\nEND\n\n";
    }
    return s;
}

} // namespace

TEST_CASE("Chat windows: WoW.exe's defaults and its chat-cache.txt byte for byte") {
    ChatWindows w;
    // ChatFrame1: SYSTEM..LOOT; ChatFrame2: the table's defaults from COMBAT_MISC_INFO on (MONEY, COMBAT_XP_GAIN,
    // COMBAT_FACTION_CHANGE, the player's and its pet's combat; not the party's or the creatures' between them).
    CHECK(w.window(0).hasGroup(game::kGroupSystem));
    CHECK(w.window(0).hasGroup(game::kGroupLoot));
    CHECK_FALSE(w.window(0).hasGroup(game::kGroupMoney));
    CHECK(w.window(1).hasGroup(game::kGroupMoney));
    CHECK(w.window(1).hasGroup(game::kGroupCombatXpGain));
    CHECK(w.window(1).hasGroup(game::chatGroupIndex("COMBAT_SELF_HITS")));
    CHECK_FALSE(w.window(1).hasGroup(game::chatGroupIndex("COMBAT_PARTY_HITS")));
    CHECK(w.window(1).anyCombat());
    CHECK_FALSE(w.window(1).combatOthers());
    CHECK_FALSE(w.window(0).anyCombat());
    std::array<u8, game::kChatWindowSlots> docked{};
    REQUIRE(w.docked(docked) == 2);
    CHECK(docked[0] == 0);
    CHECK(docked[1] == 1);
    CHECK(w.active() == 2);
    CHECK(w.format() == defaultFile());

    // Read back: the same text.
    ChatWindows read;
    REQUIRE(read.parse(defaultFile()));
    CHECK(read.format() == defaultFile());
    CHECK_FALSE(read.parse("   \n"));

    // A window of the player's with a name, a size, a colour, channels; CRLF lines as a PC's file may have.
    std::string mine = defaultFile();
    const std::string third = "WINDOW 3\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 0\nDOCKED 0\nSHOWN 0\n\nMESSAGES\nEND\n\n"
                              "CHANNELS\nEND\n\nZONECHANNELS 0\n\nEND\n\n";
    const std::string mineThird = "WINDOW 3\nNAME Trade and LFG\nSIZE 16\nCOLOR 10 20 30 128\nLOCKED 0\nDOCKED 3\n"
                                  "SHOWN 1\n\nMESSAGES\nWHISPER\nEND\n\nCHANNELS\nTrade\nLookingForGroup\nEND\n\n"
                                  "ZONECHANNELS 3\n\nEND\n\n";
    mine.replace(mine.find(third), third.size(), mineThird);
    std::string crlf;
    for (const char c : mine) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    REQUIRE(read.parse(crlf));
    const game::ChatWindowConfig& t = read.window(2);
    CHECK(t.name == "Trade and LFG");
    CHECK(t.fontSize == 16);
    CHECK(t.r == 10);
    CHECK(t.g == 20);
    CHECK(t.b == 30);
    CHECK(t.a == 128);
    CHECK(t.docked == 3);
    CHECK(t.shown);
    CHECK(t.hasGroup(game::kGroupWhisper));
    CHECK(t.hasChannel("trade")); // as ChatFrame_OnEvent: strupper
    CHECK(t.hasChannel("LookingForGroup"));
    CHECK_FALSE(t.hasChannel("General"));
    CHECK(t.zoneChannels == 3);
    CHECK(read.format() == mine);
}

TEST_CASE("Chat windows: an older file gets the groups added since (ADDEDVERSION)") {
    // 0x49967C: COMBAT_FACTION_CHANGE (added in 1) and MONEY (2) go to ChatFrame2 when the file is older.
    std::string old = defaultFile();
    old.replace(old.find("ADDEDVERSION 2"), 14, "ADDEDVERSION 1");
    const std::string money = "MONEY\n";
    old.erase(old.find(money), money.size());
    ChatWindows w;
    REQUIRE(w.parse(old));
    CHECK(w.window(1).hasGroup(game::kGroupMoney));
    old.replace(old.find("ADDEDVERSION 1"), 14, "ADDEDVERSION 2");
    REQUIRE(w.parse(old));
    CHECK_FALSE(w.window(1).hasGroup(game::kGroupMoney)); // a current file keeps what it says
}

TEST_CASE("Chat windows: FCF_OpenNewWindow, FCF_Close, the filters and the menus' labels") {
    ChatWindows w;
    const i32 opened = w.openNew("Whispers");
    REQUIRE(opened == 2);
    const game::ChatWindowConfig& n = w.window(2);
    CHECK(n.name == "Whispers");
    CHECK(n.a == 64); // DEFAULT_CHATFRAME_ALPHA
    CHECK(n.shown);
    CHECK(n.docked == 3);
    CHECK_FALSE(n.locked);
    for (const u8 g : {game::kGroupSay, game::kGroupYell, game::kGroupGuild, game::kGroupWhisper, game::kGroupParty,
                       game::kGroupChannel}) {
        CHECK(n.hasGroup(g));
    }
    CHECK_FALSE(n.hasGroup(game::kGroupSystem));
    CHECK(w.changed());
    CHECK(w.active() == 3);
    // Up to NUM_CHAT_WINDOWS: the 7th is the last one; then the last is taken again.
    for (u32 i = 0; i < 4; ++i) {
        CHECK(w.openNew({}) == static_cast<i32>(3 + i));
    }
    CHECK(w.active() == game::kChatWindows);
    CHECK(w.openNew("Again") == 6);
    // FCF_Close: out of the dock, the others close up; ChatFrame1 cannot be closed.
    w.close(2);
    CHECK_FALSE(w.window(2).shown);
    CHECK(w.window(2).docked == 0);
    std::array<u8, game::kChatWindowSlots> docked{};
    REQUIRE(w.docked(docked) == 6);
    CHECK(docked[2] == 3);
    CHECK(w.window(3).docked == 3);
    w.close(0);
    CHECK(w.window(0).shown);
    // The filters.
    w.toggleGroup(1, game::kGroupMoney);
    CHECK_FALSE(w.window(1).hasGroup(game::kGroupMoney));
    w.toggleChannel(0, "Trade");
    CHECK(w.window(0).hasChannel("Trade"));
    w.toggleChannel(0, "TRADE");
    CHECK_FALSE(w.window(0).hasChannel("Trade"));
    // FCFDropDown_LoadChatTypes' labels: the group's own name for groups of several types, else CHAT_MSG_<name>.
    CHECK(std::string(game::chatGroupLabelKey(game::kGroupSay)) == "SAY");
    CHECK(std::string(game::chatGroupLabelKey(game::kGroupYell)) == "CHAT_MSG_YELL");
    CHECK(std::string(game::chatGroupLabelKey(game::chatGroupIndex("COMBAT_SELF_HITS"))) ==
          "CHAT_MSG_COMBAT_SELF_HITS");
    CHECK(game::chatMenuGroups(game::ChatMenu::Combat).size() == 22);
    CHECK(game::chatMenuGroups(game::ChatMenu::Spell).size() == 16);
    CHECK(game::chatMenuGroups(game::ChatMenu::SpellOther).size() == 9);
    CHECK(game::chatMenuGroups(game::ChatMenu::Periodic).size() == 10);
    CHECK(game::chatGroupIndex("NOPE") == -1);
}

TEST_CASE("Mail: InboxItemCanDelete, and the popups that ask before deleting") {
    game::MailMessage m;
    m.type = net::kMailNormal;
    m.sender = 0x12;
    CHECK(m.canDelete()); // a player's letter with nothing in it
    m.money = 50;
    CHECK_FALSE(m.canDelete()); // ...with money: Return
    CHECK(m.canReturn());
    m.flags = net::kMailFlagReturned;
    CHECK(m.canDelete()); // came back: deleted with its money (DELETE_MONEY)
    CHECK_FALSE(m.canReturn());
    m.flags = net::kMailFlagCodPayment;
    CHECK(m.canDelete());
    game::MailMessage ah;
    ah.type = 2; // an auction house's
    ah.item.entry = 2589;
    CHECK(ah.canDelete()); // DELETE_MAIL asks first

    // StaticPopup.lua's entries.
    const PopupInfo& withItems = popupInfo(PopupKind::AbandonQuestWithItems);
    CHECK(std::string(withItems.text.name) == "ABANDON_QUEST_CONFIRM_WITH_ITEMS");
    CHECK(withItems.has(PopupInfo::kExclusive));
    CHECK(withItems.has(PopupInfo::kWhileDead));
    const PopupInfo& mail = popupInfo(PopupKind::DeleteMail);
    CHECK(std::string(mail.text.name) == "DELETE_MAIL_CONFIRMATION");
    CHECK(mail.has(PopupInfo::kShowAlert));
    CHECK(popupInfo(PopupKind::DeleteMoney).has(PopupInfo::kMoneyFrame));
    CHECK(popupInfo(PopupKind::NameChat).has(PopupInfo::kEditBox));
}
