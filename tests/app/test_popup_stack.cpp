// The StaticPopup stack (parity pass, popups): StaticPopup.lua's rules as app/PopupStack runs them, without
// drawing: four slots and the refusal of a fifth, the same kind replaced in its own slot, exclusive popups,
// `cancels`, CAMP / QUIT and DEATH closing the others, the dead player's refusals, timeouts, start delays,
// the twice-asked XP_LOSS, RECOVER_CORPSE staying after its OKAY, Escape, StaticPopup_Hide, and the order
// outcomes come out in.
#include "app/PopupStack.h"

#include <doctest/doctest.h>

#include <ostream>
#include <vector>

namespace {

using namespace vd;

std::vector<PopupOutcome> drain(PopupStack& s) {
    std::vector<PopupOutcome> out;
    PopupOutcome o;
    while (s.takeOutcome(o)) {
        out.push_back(o);
    }
    return out;
}

PopupKind kindAt(const PopupStack& s, u32 index) {
    return s.slots()[index].kind;
}

} // namespace

TEST_CASE("Popup stack: four slots, the first free one, a fifth refused") {
    PopupStack s;
    CHECK_FALSE(s.any());
    REQUIRE(s.show(PopupKind::PartyInvite, false));
    REQUIRE(s.show(PopupKind::GuildInvite, false));
    REQUIRE(s.show(PopupKind::DuelRequested, false, 7));
    REQUIRE(s.show(PopupKind::ConfirmBinder, false, 9));
    CHECK(s.count() == 4);
    CHECK(kindAt(s, 0) == PopupKind::PartyInvite);
    CHECK(kindAt(s, 3) == PopupKind::ConfirmBinder);
    CHECK(drain(s).empty());
    // A fifth: OnCancel() at once (a trade asked while four are up is declined).
    CHECK(s.show(PopupKind::Trade, false, 5) == nullptr);
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::Trade);
    CHECK(o[0].end == PopupEnd::Refused);
    CHECK(o[0].data == 5);
    CHECK_FALSE(o[0].shown);
    // The second goes: its frame frees, the others keep their slots (StaticPopup3 stays under StaticPopup2's
    // place).
    s.click(1, 2);
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::GuildInvite);
    CHECK(o[0].end == PopupEnd::Cancel);
    CHECK(o[0].shown);
    CHECK_FALSE(s.slots()[1].shown());
    CHECK(kindAt(s, 2) == PopupKind::DuelRequested);
    // The next one takes that free slot.
    REQUIRE(s.show(PopupKind::Trade, false, 5));
    CHECK(kindAt(s, 1) == PopupKind::Trade);
}

TEST_CASE("Popup stack: the same kind is replaced in its own slot") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::ConfirmBinder, false, 1));
    REQUIRE(s.show(PopupKind::PartyInvite, false));
    Popup* again = s.show(PopupKind::ConfirmBinder, false, 2);
    REQUIRE(again);
    CHECK(kindAt(s, 0) == PopupKind::ConfirmBinder);
    CHECK(s.slots()[0].data == 2);
    const std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Override);
    CHECK(o[0].data == 1);
}

TEST_CASE("Popup stack: exclusive popups replace one another") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::PartyInvite, false));     // not exclusive: stays
    REQUIRE(s.show(PopupKind::AbandonQuest, false, 3)); // exclusive
    REQUIRE(s.show(PopupKind::LootBind, false, 4));     // exclusive: ABANDON_QUEST goes
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::AbandonQuest);
    CHECK(o[0].end == PopupEnd::Override);
    CHECK(s.visible(PopupKind::PartyInvite));
    CHECK(s.visible(PopupKind::LootBind));
    CHECK(kindAt(s, 1) == PopupKind::LootBind); // into the freed slot
}

TEST_CASE("Popup stack: RESURRECT cancels DEATH, DEATH cancels RECOVER_CORPSE") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::Death, true));
    REQUIRE(s.show(PopupKind::Resurrect, true, 0x33));
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::Death);
    CHECK(o[0].end == PopupEnd::Override);
    REQUIRE(s.show(PopupKind::RecoverCorpse, true));
    REQUIRE(s.show(PopupKind::Death, true));
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::RecoverCorpse);
    CHECK(s.visible(PopupKind::Resurrect)); // whileDead: DEATH leaves it
}

TEST_CASE("Popup stack: CAMP closes what logging out may close, DEATH what the dead may not see") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::PartyInvite, false));
    REQUIRE(s.show(PopupKind::ConfirmSummon, false, 8)); // notClosableByLogout
    REQUIRE(s.show(PopupKind::Trade, false, 5));
    REQUIRE(s.show(PopupKind::Camp, false));
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 2);
    CHECK(o[0].kind == PopupKind::PartyInvite);
    CHECK(o[0].end == PopupEnd::Override);
    CHECK(o[1].kind == PopupKind::Trade);
    CHECK(s.visible(PopupKind::ConfirmSummon));
    CHECK(s.visible(PopupKind::Camp));
    CHECK(kindAt(s, 0) == PopupKind::Camp); // the first free slot

    PopupStack d;
    REQUIRE(d.show(PopupKind::DuelRequested, false, 1)); // not whileDead
    REQUIRE(d.show(PopupKind::GuildInvite, false));      // whileDead
    REQUIRE(d.show(PopupKind::Death, true));
    o = drain(d);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::DuelRequested);
    CHECK(d.visible(PopupKind::GuildInvite));
    // Dead: what is not whileDead is refused at once (OnCancel: the duel and the trade are declined).
    CHECK(d.show(PopupKind::Trade, true, 5) == nullptr);
    o = drain(d);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Refused);
    CHECK(d.show(PopupKind::PartyInvite, true)); // whileDead
}

TEST_CASE("Popup stack: timeouts and start delays") {
    PopupStack s;
    Popup* invite = s.show(PopupKind::PartyInvite, false);
    REQUIRE(invite);
    CHECK(invite->timeLeftMs == 60000); // timeout = 60
    Popup* resurrect = s.show(PopupKind::ResurrectNoSickness, true, 0x33);
    REQUIRE(resurrect);
    resurrect->startDelayMs = 10000; // the corpse delay (StartDelay)
    resurrect->timeLeftMs = 70000;   // OnShow: the delay + 60
    CHECK_FALSE(PopupStack::button1Ready(*resurrect));
    s.click(1, 1); // Accept is disabled
    CHECK(drain(s).empty());
    s.update(10000);
    CHECK(PopupStack::button1Ready(s.slots()[1]));
    s.update(49999);
    CHECK(s.visible(PopupKind::PartyInvite));
    s.update(1);
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::PartyInvite);
    CHECK(o[0].end == PopupEnd::Timeout);
    CHECK(o[0].timeLeftMs == 0);
    s.click(1, 1);
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::ResurrectNoSickness);
    CHECK(o[0].end == PopupEnd::Accept);
    CHECK(o[0].data == 0x33);
    CHECK(o[0].timeLeftMs == 10000);
    // No timeout: stays.
    REQUIRE(s.show(PopupKind::AbandonQuest, false, 3));
    s.update(3600000);
    CHECK(s.visible(PopupKind::AbandonQuest));
}

TEST_CASE("Popup stack: CAMP's countdown reports the time left when it is hidden") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::Camp, false));
    s.update(5000);
    s.click(0, 1); // its only button, CANCEL
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Accept);
    CHECK(o[0].timeLeftMs == 15000);
    REQUIRE(s.show(PopupKind::Camp, false));
    s.update(20000);
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Timeout); // logged out: OnHide must not cancel
    CHECK(o[0].timeLeftMs == 0);
}

TEST_CASE("Popup stack: XP_LOSS asks twice, RECOVER_CORPSE stays after OKAY") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::XpLoss, true, 0x44));
    s.click(0, 1);
    CHECK(drain(s).empty());
    CHECK(s.slots()[0].again); // CONFIRM_XP_LOSS_AGAIN now
    s.click(0, 1);
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Accept);
    CHECK_FALSE(s.any());

    REQUIRE(s.show(PopupKind::RecoverCorpse, true));
    s.click(0, 1);
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Accept);
    CHECK(o[0].stays);
    CHECK(s.visible(PopupKind::RecoverCorpse));
    s.hide(PopupKind::RecoverCorpse); // CORPSE_OUT_OF_RANGE / alive
    o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].end == PopupEnd::Hidden);
}

TEST_CASE("Popup stack: Escape, StaticPopup_Hide by data, clear") {
    PopupStack s;
    REQUIRE(s.show(PopupKind::Death, true));                      // no hideOnEscape
    REQUIRE(s.show(PopupKind::PartyInvite, true));                // hideOnEscape
    REQUIRE(s.show(PopupKind::DuelOutOfBounds, true) == nullptr); // not whileDead: refused
    drain(s);
    CHECK(s.escape());
    std::vector<PopupOutcome> o = drain(s);
    REQUIRE(o.size() == 1);
    CHECK(o[0].kind == PopupKind::PartyInvite);
    CHECK(o[0].end == PopupEnd::Cancel);
    CHECK_FALSE(s.escape());
    CHECK(s.visible(PopupKind::Death));

    REQUIRE(s.show(PopupKind::ConfirmBinder, false, 1));
    s.hide(PopupKind::ConfirmBinder, 2); // another innkeeper's: stays
    CHECK(s.visible(PopupKind::ConfirmBinder));
    s.hide(PopupKind::ConfirmBinder, 1);
    CHECK_FALSE(s.visible(PopupKind::ConfirmBinder));
    s.clear();
    CHECK_FALSE(s.any());
    o = drain(s);
    REQUIRE(o.size() == 2);
    CHECK(o[1].kind == PopupKind::Death);
    CHECK(o[1].end == PopupEnd::Hidden);
}

TEST_CASE("Popup stack: the table follows StaticPopupDialogs") {
    CHECK(popupInfo(PopupKind::Camp).timeoutMs == 20000);
    CHECK(popupInfo(PopupKind::Quit).timeoutMs == 20000);
    CHECK(popupInfo(PopupKind::PartyInvite).timeoutMs == 60000);
    CHECK(popupInfo(PopupKind::DuelOutOfBounds).timeoutMs == 10000);
    CHECK(popupInfo(PopupKind::Death).button2.name == "USE_SOULSTONE");
    CHECK(popupInfo(PopupKind::Quit).button1.name == "QUIT_NOW");
    CHECK(popupInfo(PopupKind::Camp).button2.name.empty()); // CAMP_NOW is commented out in 1.12
    CHECK(popupInfo(PopupKind::DeleteGoodItem).has(PopupInfo::kEditBox));
    CHECK(popupInfo(PopupKind::DeleteItem).has(PopupInfo::kShowAlert));
    CHECK(popupInfo(PopupKind::ConfirmTalentWipe).has(PopupInfo::kMoneyFrame));
    CHECK(popupInfo(PopupKind::Resurrect).cancels == PopupKind::Death);
    CHECK(popupInfo(PopupKind::Death).cancels == PopupKind::RecoverCorpse);
    CHECK(popupInfo(PopupKind::ConfirmSummon).has(PopupInfo::kNotClosableByLogout));
    CHECK_FALSE(popupInfo(PopupKind::DuelRequested).has(PopupInfo::kWhileDead));
    CHECK(popupInfo(PopupKind::XpLoss).againText.name == "CONFIRM_XP_LOSS_AGAIN");
    CHECK(popupInfo(PopupKind::EquipBind).text.name == "EQUIP_NO_DROP");
}
