// Phase 20 (edit, docs/MOBILE_CONTROLS.md §5.3, §5.5, §9): what the template list and the edit mode change in a touch
// layout (game/TouchLayoutEdit): places on a 1 mm grid, clear of each other and inside the screen, Main moving the
// whole cluster, sizes and scale in their ranges; actions put, hidden, inherited and swapped by hand in the base and
// in a layer; groups made and changed; templates added, named, deleted, restored from the class preset and given the
// same places. And the server's copy in account data type 6 with WoW.exe's rules (game::AccountData) and this client's
// choice (app/AccountDataFiles): a newer copy of ours is taken, 1.12's own layout-cache.txt is not (ours goes up
// again), a change made here wins.
#include "game/AccountData.h"
#include "game/TouchLayout.h"
#include "game/TouchLayoutEdit.h"
#include "net/AccountDataMessages.h"

#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ostream> // doctest prints strings with it
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace vd;
using namespace vd::game;

namespace {

constexpr u32 slot(TouchPlace p) {
    return touchIndex(p);
}

bool clearOfAll(const TouchGeometry& g, TouchPlace p) {
    for (u32 q = 0; q < kTouchPlaces; ++q) {
        if (touchPlacesOverlap(g, p, static_cast<TouchPlace>(q))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("touch edit: a place moves on the 1 mm grid, clear of the others and inside the screen") {
    TouchGeometry g = defaultTouchGeometry(TouchProfile::Square);
    const TouchArea none{};
    // Somewhere free: snapped.
    CHECK(moveTouchPlace(g, TouchPlace::ColumnH, -50.3f, -20.6f, none));
    CHECK(g.places[slot(TouchPlace::ColumnH)].x == doctest::Approx(-50.0f));
    CHECK(g.places[slot(TouchPlace::ColumnH)].y == doctest::Approx(-21.0f));
    // Onto another: the nearest free point instead, never overlapping.
    const TouchPlacement column = g.places[slot(TouchPlace::ColumnG)];
    CHECK(moveTouchPlace(g, TouchPlace::ColumnH, column.x, column.y, none));
    const TouchPlacement& moved = g.places[slot(TouchPlace::ColumnH)];
    CHECK(clearOfAll(g, TouchPlace::ColumnH));
    CHECK(std::hypot(moved.x - column.x, moved.y - column.y) <= 15.0f);
    CHECK(moved.x == std::round(moved.x));
    CHECK(moved.y == std::round(moved.y));
    // Past the screen's edge: the nearest point with the whole circle inside.
    const TouchArea area{-60.0f, 30.0f, -26.0f, 60.0f};
    CHECK(moveTouchPlace(g, TouchPlace::ColumnH, -58.0f, -24.0f, area));
    CHECK(g.places[slot(TouchPlace::ColumnH)].x == doctest::Approx(-55.0f));
    CHECK(g.places[slot(TouchPlace::ColumnH)].y == doctest::Approx(-21.0f));
    // Where it already is: nothing moved.
    CHECK_FALSE(moveTouchPlace(g, TouchPlace::ColumnH, -55.2f, -20.9f, area));
    // A place never shown does not move.
    g.places[slot(TouchPlace::Pet)].size = 0.0f;
    CHECK_FALSE(moveTouchPlace(g, TouchPlace::Pet, 0.0f, 50.0f, none));
}

TEST_CASE("touch edit: dragging Main moves the whole cluster, kept inside the screen") {
    TouchGeometry g = defaultTouchGeometry(TouchProfile::Square);
    const TouchGeometry before = g;
    // How far right the cluster reaches around Main (hundredths of the screen's height).
    f32 maxX = 0.0f;
    for (const TouchPlacement& at : g.places) {
        maxX = at.size > 0.0f ? std::max(maxX, at.x + at.size * 0.5f) : maxX;
    }
    const TouchArea area{-80.0f, 30.0f, -30.0f, 100.0f};
    CHECK(moveTouchPlace(g, TouchPlace::Main, 5.4f, 3.6f, area));
    CHECK(g.mainRight == doctest::Approx(before.mainRight - 5.0f));  // 5 toward the middle of the screen
    CHECK(g.mainBottom == doctest::Approx(before.mainBottom + 4.0f)); // 4 up
    for (u32 p = 0; p < kTouchPlaces; ++p) {
        CHECK(g.places[p].x == before.places[p].x); // the others keep their places around Main
        CHECK(g.places[p].y == before.places[p].y);
    }
    // Further than the area lets the cluster go: as far as it fits (whole grid steps of it).
    const f32 right = g.mainRight;
    CHECK(moveTouchPlace(g, TouchPlace::Main, 20.0f, 0.0f, area));
    CHECK(g.mainRight == doctest::Approx(right - std::floor(30.0f - maxX)));
}

TEST_CASE("touch edit: sizes stay within 70..150 % and never grow over a neighbour; the scale within 70..130 %") {
    TouchGeometry g = defaultTouchGeometry(TouchProfile::Square);
    CHECK(touchPlaceSizePercent(g, TouchProfile::Square, TouchPlace::RingA) == doctest::Approx(100.0f));
    // A ring button has room for 150 %.
    CHECK(resizeTouchPlace(g, TouchProfile::Square, TouchPlace::RingA, 150.0f) == doctest::Approx(150.0f));
    CHECK(g.places[slot(TouchPlace::RingA)].size == doctest::Approx(9.5f * 1.5f));
    CHECK(clearOfAll(g, TouchPlace::RingA));
    // Main grows only until it would touch Reactive.
    const f32 main = resizeTouchPlace(g, TouchProfile::Square, TouchPlace::Main, 150.0f);
    CHECK(main > 100.0f);
    CHECK(main < 150.0f);
    CHECK(clearOfAll(g, TouchPlace::Main));
    // Shrinking always works, down to 70 %.
    CHECK(resizeTouchPlace(g, TouchProfile::Square, TouchPlace::Main, 20.0f) == doctest::Approx(70.0f));
    CHECK(g.places[slot(TouchPlace::Main)].size == doctest::Approx(12.4f * 0.7f));
    setTouchScale(g, 2.0f);
    CHECK(g.scale == doctest::Approx(kTouchMaxScale));
    setTouchScale(g, 0.1f);
    CHECK(g.scale == doctest::Approx(kTouchMinScale));
    resetTouchGeometry(g, TouchProfile::Square);
    CHECK(g.scale == doctest::Approx(1.0f));
    CHECK(g.places[slot(TouchPlace::Main)].size == doctest::Approx(12.4f));
}

TEST_CASE("touch edit: actions put, hidden, inherited and swapped by hand, in the base and in a layer") {
    TouchTemplate t = makeTouchTemplate("Solo");
    const u32 p = slot(TouchPlace::Main), a = slot(TouchPlace::RingA);
    t.layers[0][p] = TouchAction::spell(1752); // a preset's (automatic)
    putTouchAction(t, TouchContext::Base, a, TouchAction::spell(2098));
    CHECK(t.layers[0][a].manual());
    CHECK_FALSE(t.layers[0][p].manual());
    putTouchAction(t, TouchContext::Stealth, p, TouchAction::spell(8676));
    CHECK(resolveTouchAction(t, TouchContext::Stealth, p).id == 8676);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, a).id == 2098); // inherited
    bool inherited = false;
    CHECK(touchSlotShown(t, TouchContext::Stealth, a, inherited).id == 2098);
    CHECK(inherited);
    // Hidden in the layer: nothing there; inherited again: the base's.
    hideTouchSlot(t, TouchContext::Stealth, a);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, a).empty());
    CHECK(touchSlotShown(t, TouchContext::Stealth, a, inherited).hidden());
    CHECK_FALSE(inherited);
    inheritTouchSlot(t, TouchContext::Stealth, a);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, a).id == 2098);
    // Swapped in the layer: what each shows changes places; one that ends up showing the base's own inherits it.
    swapTouchSlots(t, TouchContext::Stealth, p, a);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, p).id == 2098);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, a).id == 8676);
    CHECK(t.layers[static_cast<u32>(TouchContext::Stealth)][p].manual());
    swapTouchSlots(t, TouchContext::Stealth, p, a);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, p).id == 8676);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, a).id == 2098);
    CHECK(t.layers[static_cast<u32>(TouchContext::Stealth)][a].empty()); // inherits again
    CHECK_FALSE(t.layers[static_cast<u32>(TouchContext::Stealth)][a].hidden());
    // The base: swapped as they are, both the player's now.
    swapTouchSlots(t, TouchContext::Base, p, a);
    CHECK(t.layers[0][p].id == 2098);
    CHECK(t.layers[0][a].id == 1752);
    CHECK(t.layers[0][a].manual());
    // Emptied in the base: hidden by hand (shows nothing, the presets leave it).
    putTouchAction(t, TouchContext::Base, a, {});
    CHECK(t.layers[0][a].hidden());
    CHECK(t.layers[0][a].manual());
    CHECK(resolveTouchAction(t, TouchContext::Base, a).empty());
    inheritTouchSlot(t, TouchContext::Base, a); // the base inherits from nothing
    CHECK(t.layers[0][a].hidden());
    // A fixed rank stays with the action put.
    putTouchAction(t, TouchContext::Base, p, TouchAction::spell(1757, kTouchFixedRank));
    CHECK((t.layers[0][p].flags & kTouchFixedRank) != 0);
    CHECK(t.layers[0][p].manual());
}

TEST_CASE("touch edit: groups made by hand, changed in place, and the free indices") {
    TouchTemplate t = makeTouchTemplate("Solo");
    const u32 f = slot(TouchPlace::ColumnF), g = slot(TouchPlace::ColumnG);
    const TouchAction a = TouchAction::spell(6673), b = TouchAction::spell(1160), c = TouchAction::item(5512);
    const TouchAction list[] = {a, {}, b, {TouchActionKind::Group, 0, 0, 3}, c};
    CHECK(putTouchGroup(t, TouchContext::Base, f, list) == 0);
    CHECK(t.layers[0][f].kind == TouchActionKind::Group);
    CHECK(t.layers[0][f].id == 0);
    CHECK(t.layers[0][f].manual());
    REQUIRE(t.groups[0].count == 3); // the empty one and the group left out
    CHECK(t.groups[0].actions[2] == c);
    CHECK((t.groups[0].flags & kTouchManual) != 0);
    // Changed in place: the same group, its current one kept where it still is.
    t.groups[0].current = 1; // b
    const TouchAction again[] = {c, b};
    CHECK(putTouchGroup(t, TouchContext::Base, f, again) == 0);
    CHECK(t.groups[0].count == 2);
    CHECK(t.groups[0].current == 1);
    // Another slot: another group; nothing to make it of: none.
    const TouchAction one[] = {a};
    CHECK(putTouchGroup(t, TouchContext::Base, g, one) == 1);
    CHECK(putTouchGroup(t, TouchContext::Base, g, std::span<const TouchAction>{}) == -1);
    // At most eight.
    TouchAction many[10];
    for (u32 i = 0; i < 10; ++i) {
        many[i] = TouchAction::spell(100 + i);
    }
    CHECK(putTouchGroup(t, TouchContext::Base, g, many) == 1);
    CHECK(t.groups[1].count == TouchGroup::kMaxActions);
    // A layer's slot that inherits the base's group gets a group of its own (the base's stays).
    CHECK(touchSlotGroup(t, TouchContext::Stealth, f) == 0);
    CHECK(putTouchGroup(t, TouchContext::Stealth, f, one) == 2);
    CHECK(t.groups[0].count == 2);
    CHECK(resolveTouchAction(t, TouchContext::Stealth, f).id == 2);
    // Free indices: never an automatic one (the presets', even unused); a hand-made one nothing shows any more.
    for (u32 i = 3; i < kTouchGroups; ++i) {
        t.groups[i].autoKind = 7;
    }
    CHECK(freeTouchGroup(t) == -1);
    putTouchAction(t, TouchContext::Base, g, TouchAction::spell(1));
    CHECK_FALSE(touchGroupUsed(t, 1));
    CHECK(freeTouchGroup(t) == 1);
    t.classBarGroups[0] = 2; // group 1 shown by the class bar
    CHECK(touchGroupUsed(t, 1));
    CHECK(freeTouchGroup(t) == -1);
}

TEST_CASE("touch edit: templates added, named, deleted, restored from the preset, given the same places") {
    TouchLayout layout;
    CHECK(addTouchTemplate(layout, makeTouchTemplate("Solo")) == 0);
    std::array<char, kTouchNameBytes> name{};
    uniqueTouchTemplateName(layout, "Solo", name);
    CHECK(std::string_view(name.data()) == "Solo 2");
    CHECK(addTouchTemplate(layout, makeTouchTemplate(name.data())) == 1);
    uniqueTouchTemplateName(layout, "Solo 2", name); // a copy of a copy: the next number
    CHECK(std::string_view(name.data()) == "Solo 3");
    uniqueTouchTemplateName(layout, "Raid", name);
    CHECK(std::string_view(name.data()) == "Raid");
    // Long names keep room for the number, never cutting a UTF-8 character.
    const std::string longName = std::string(20, 'a') + "\xC3\xB3" + "b"; // 23 bytes
    addTouchTemplate(layout, makeTouchTemplate(longName));
    uniqueTouchTemplateName(layout, longName, name);
    const std::string_view made(name.data());
    CHECK(made.size() <= kTouchNameBytes - 1);
    CHECK(made == std::string(20, 'a') + " 2");
    while (addTouchTemplate(layout, makeTouchTemplate("X")) >= 0) {
    }
    CHECK(layout.count == kMaxTouchTemplates);
    // Deleting keeps the same template active (or the next when the active one went).
    layout.templates[3].setName("Active");
    layout.active = 3;
    CHECK(removeTouchTemplate(layout, 1));
    CHECK(layout.count == kMaxTouchTemplates - 1);
    CHECK(layout.current().nameView() == "Active");
    CHECK(removeTouchTemplate(layout, layout.active));
    CHECK(layout.active == 2);
    CHECK(layout.current().nameView() == "X");
    CHECK_FALSE(removeTouchTemplate(layout, 99));
    while (layout.count > 1) {
        CHECK(removeTouchTemplate(layout, 0));
    }
    CHECK_FALSE(removeTouchTemplate(layout, 0)); // never the last one
    CHECK(layout.active == 0);

    // Restore: the preset's actions, groups, layers and role; the player's name, places and site stay.
    TouchLayout presets;
    TouchTemplate damage = makeTouchTemplate("Damage");
    damage.role = 1;
    damage.layers[0][slot(TouchPlace::Main)] = TouchAction::spell(585);
    TouchTemplate heal = makeTouchTemplate("Heal");
    heal.role = 2;
    heal.layers[0][slot(TouchPlace::Main)] = TouchAction::spell(2061);
    heal.groups[0].count = 1;
    heal.groups[0].actions[0] = TouchAction::spell(1243);
    heal.disabledLayers = 1u << static_cast<u32>(TouchContext::Shadowform);
    addTouchTemplate(presets, damage);
    addTouchTemplate(presets, heal);
    TouchTemplate mine = makeTouchTemplate("Mine");
    mine.role = 2;
    mine.site = static_cast<u8>(TouchSite::Dungeon);
    mine.geometry[0].scale = 1.2f;
    putTouchAction(mine, TouchContext::Base, slot(TouchPlace::Main), TouchAction::spell(9999));
    CHECK(restoreTouchTemplate(mine, presets));
    CHECK(mine.layers[0][slot(TouchPlace::Main)].id == 2061);
    CHECK(mine.groups[0].count == 1);
    CHECK(mine.disabledLayers == heal.disabledLayers);
    CHECK(mine.nameView() == "Mine");
    CHECK(mine.geometry[0].scale == doctest::Approx(1.2f));
    CHECK(mine.site == static_cast<u8>(TouchSite::Dungeon));
    mine.role = 0; // made from the bars: the class's first
    CHECK(restoreTouchTemplate(mine, presets));
    CHECK(mine.layers[0][slot(TouchPlace::Main)].id == 585);
    CHECK_FALSE(restoreTouchTemplate(mine, TouchLayout{}));

    // The same places for all: both profiles of the one in use, the actions untouched.
    TouchLayout two;
    addTouchTemplate(two, makeTouchTemplate("A"));
    addTouchTemplate(two, makeTouchTemplate("B"));
    two.templates[1].layers[0][0] = TouchAction::spell(1);
    two.templates[0].geometry[1].scale = 0.8f;
    moveTouchPlace(two.templates[0].geometry[0], TouchPlace::ColumnH, -50.0f, -21.0f, {});
    copyTouchGeometryToAll(two, 0);
    CHECK(two.templates[1].geometry[1].scale == doctest::Approx(0.8f));
    CHECK(two.templates[1].geometry[0].places[slot(TouchPlace::ColumnH)].x == doctest::Approx(-50.0f));
    CHECK(two.templates[1].layers[0][0].id == 1);
}

TEST_CASE("touch edit: what the edit mode set survives the file") {
    TouchLayout layout;
    TouchTemplate t = makeTouchTemplate("Solo");
    putTouchAction(t, TouchContext::Base, slot(TouchPlace::Main), TouchAction::spell(1757, kTouchFixedRank));
    putTouchAction(t, TouchContext::Stealth, slot(TouchPlace::Main), TouchAction::spell(8676));
    hideTouchSlot(t, TouchContext::Stealth, slot(TouchPlace::RingB));
    const TouchAction list[] = {TouchAction::spell(6673), TouchAction::spell(1160)};
    putTouchGroup(t, TouchContext::Base, slot(TouchPlace::ColumnF), list);
    moveTouchPlace(t.geometry[0], TouchPlace::ColumnH, -50.0f, -21.0f, {});
    resizeTouchPlace(t.geometry[1], TouchProfile::Wide, TouchPlace::Pet, 120.0f);
    t.disabledLayers = 1u << static_cast<u32>(TouchContext::Prowl);
    addTouchTemplate(layout, t);
    TouchLayout back;
    REQUIRE(parseTouchLayout(formatTouchLayout(layout), back));
    const TouchTemplate& b = back.templates[0];
    CHECK(b.layers[0][slot(TouchPlace::Main)] == t.layers[0][slot(TouchPlace::Main)]); // manual, fixed rank
    CHECK(b.layers[static_cast<u32>(TouchContext::Stealth)][slot(TouchPlace::Main)].manual());
    CHECK(b.layers[static_cast<u32>(TouchContext::Stealth)][slot(TouchPlace::RingB)].hidden());
    CHECK(b.layers[0][slot(TouchPlace::ColumnF)].kind == TouchActionKind::Group);
    CHECK((b.groups[0].flags & kTouchManual) != 0);
    CHECK(b.groups[0].count == 2);
    CHECK(b.geometry[0].places[slot(TouchPlace::ColumnH)].x == doctest::Approx(-50.0f));
    CHECK(b.geometry[1].places[slot(TouchPlace::Pet)].size == doctest::Approx(7.0f * 1.2f));
    CHECK(b.disabledLayers == t.disabledLayers);
}

namespace {

// A device's touch layout behind account data type 6, as app/AccountDataFiles keeps it: read = the layout in use (its
// file before it is read); write = ours replaces it, 1.12's layout-cache.txt does not (ours goes up again).
struct Device {
    TouchLayout layout;
    bool loaded = true;
    std::string file;
    bool reupload = false;
    std::vector<std::pair<net::Opcode, std::vector<u8>>> sent;

    AccountDataStore store() {
        AccountDataStore s;
        s.context = this;
        s.read = [](void* c, u32 type, std::string& out) {
            Device& self = *static_cast<Device*>(c);
            if (type != static_cast<u32>(net::AccountDataType::CharacterLayout)) {
                return false;
            }
            out = self.loaded && self.layout.count > 0 ? formatTouchLayout(self.layout) : self.file;
            return true;
        };
        s.write = [](void* c, u32 type, std::string_view text) {
            Device& self = *static_cast<Device*>(c);
            if (type != static_cast<u32>(net::AccountDataType::CharacterLayout)) {
                return;
            }
            if (!isTouchLayoutText(text)) {
                self.reupload = self.layout.count > 0 || !self.file.empty();
                return;
            }
            self.file.assign(text);
            parseTouchLayout(text, self.layout);
        };
        return s;
    }
    PacketSink sink() {
        PacketSink s;
        s.context = this;
        s.send = [](void* c, net::Opcode opcode, std::span<const u8> body) {
            static_cast<Device*>(c)->sent.emplace_back(opcode, std::vector<u8>(body.begin(), body.end()));
            return true;
        };
        return s;
    }
    std::vector<std::vector<u8>> sentWith(net::Opcode opcode) const {
        std::vector<std::vector<u8>> out;
        for (const auto& [op, body] : sent) {
            if (op == opcode) {
                out.push_back(body);
            }
        }
        return out;
    }
};

TouchLayout sampleLayout(std::string_view name, u32 mainSpell) {
    TouchLayout l;
    TouchTemplate t = makeTouchTemplate(name);
    putTouchAction(t, TouchContext::Base, slot(TouchPlace::Main), TouchAction::spell(mainSpell));
    addTouchTemplate(l, t);
    return l;
}

std::vector<u8> md5Packet(const std::string& layoutText) {
    std::vector<u8> out(16 * net::kAccountDataTypes, 0);
    if (!layoutText.empty()) {
        const net::Md5 d = net::md5(std::string_view(layoutText));
        std::memcpy(out.data() + 16 * 6, d.data(), 16);
    }
    return out;
}

std::vector<u8> updatePacket(std::string_view text) {
    std::vector<u8> out;
    REQUIRE(net::writeUpdateAccountData(out, 6, text));
    return out;
}

constexpr std::string_view kLayoutCache = "VERSION 1\nFRAME PlayerFrame\nPOINT TOPLEFT 10 -10\n"; // 1.12's own

} // namespace

TEST_CASE("touch edit: the server's copy (account data type 6): newer ours taken, 1.12's not, a change here wins") {
    const std::string mine = formatTouchLayout(sampleLayout("Solo", 1752));
    const std::string other = formatTouchLayout(sampleLayout("Dungeon", 2098)); // the Mi 11X's
    {
        // Untouched here since the last sync, the other phone changed it: asked for and taken.
        Device phone;
        phone.loaded = false; // at login, before the character's first frame: its file
        phone.file = mine;
        AccountData data;
        data.setSink(phone.sink());
        data.setStore(phone.store());
        AccountData::Digests synced{};
        synced[6] = net::md5(std::string_view(mine));
        data.setSynced(synced);
        REQUIRE(data.handle(net::Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(other), 0));
        const auto requests = phone.sentWith(net::Opcode::CMSG_REQUEST_ACCOUNT_DATA);
        REQUIRE(requests.size() == 1);
        CHECK(requests[0] == std::vector<u8>{6, 0, 0, 0});
        CHECK(data.waiting(6));
        CHECK(phone.sentWith(net::Opcode::CMSG_UPDATE_ACCOUNT_DATA).empty());
        REQUIRE(data.handle(net::Opcode::SMSG_UPDATE_ACCOUNT_DATA, updatePacket(other), 10));
        CHECK(phone.file == other);
        REQUIRE(phone.layout.count == 1);
        CHECK(phone.layout.templates[0].nameView() == "Dungeon");
        CHECK_FALSE(phone.reupload);
    }
    {
        // The PC's 1.12 client wrote its layout-cache.txt over it: ours stays and goes up again (after 30 s).
        Device phone;
        phone.layout = sampleLayout("Solo", 1752);
        phone.file = mine;
        AccountData data;
        data.setSink(phone.sink());
        data.setStore(phone.store());
        AccountData::Digests synced{};
        synced[6] = net::md5(std::string_view(mine));
        data.setSynced(synced);
        REQUIRE(data.handle(net::Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(std::string(kLayoutCache)), 0));
        REQUIRE(phone.sentWith(net::Opcode::CMSG_REQUEST_ACCOUNT_DATA).size() == 1);
        REQUIRE(data.handle(net::Opcode::SMSG_UPDATE_ACCOUNT_DATA, updatePacket(kLayoutCache), 10));
        CHECK(phone.file == mine);
        CHECK(phone.layout.templates[0].nameView() == "Solo");
        REQUIRE(phone.reupload);
        CHECK_FALSE(data.waiting(6));
        data.changed(6, 20); // AccountDataFiles::update
        data.update(20 + AccountData::kSaveDelayMs - 1);
        CHECK(phone.sentWith(net::Opcode::CMSG_UPDATE_ACCOUNT_DATA).empty());
        data.update(20 + AccountData::kSaveDelayMs);
        const auto uploads = phone.sentWith(net::Opcode::CMSG_UPDATE_ACCOUNT_DATA);
        REQUIRE(uploads.size() == 1);
        net::AccountDataUpdate up;
        REQUIRE(net::parseUpdateAccountData(uploads[0], up));
        CHECK(up.type == 6);
        CHECK(up.text == mine);
    }
    {
        // Changed here since the last sync: ours goes up at once, whatever the server has.
        Device phone;
        phone.layout = sampleLayout("Solo", 1752);
        phone.file = mine;
        AccountData data;
        data.setSink(phone.sink());
        data.setStore(phone.store());
        AccountData::Digests synced{};
        synced[6] = net::md5(std::string_view("VDTOUCH 1\nactive 0\n"));
        data.setSynced(synced);
        REQUIRE(data.handle(net::Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(other), 0));
        CHECK(phone.sentWith(net::Opcode::CMSG_REQUEST_ACCOUNT_DATA).empty());
        const auto uploads = phone.sentWith(net::Opcode::CMSG_UPDATE_ACCOUNT_DATA);
        REQUIRE(uploads.size() == 1);
        net::AccountDataUpdate up;
        REQUIRE(net::parseUpdateAccountData(uploads[0], up));
        CHECK(up.text == mine);
        CHECK(data.synced()[6] == net::md5(std::string_view(mine)));
    }
    {
        // A new install (no file, no cache.md5): the server's copy comes down.
        Device phone;
        phone.loaded = false;
        AccountData data;
        data.setSink(phone.sink());
        data.setStore(phone.store());
        REQUIRE(data.handle(net::Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(other), 0));
        REQUIRE(phone.sentWith(net::Opcode::CMSG_REQUEST_ACCOUNT_DATA).size() == 1);
        REQUIRE(data.handle(net::Opcode::SMSG_UPDATE_ACCOUNT_DATA, updatePacket(other), 10));
        CHECK(phone.layout.templates[0].nameView() == "Dungeon");
        // ...and 1.12's text with nothing of ours here: nothing to send back.
        Device fresh;
        fresh.loaded = false;
        AccountData empty;
        empty.setSink(fresh.sink());
        empty.setStore(fresh.store());
        REQUIRE(empty.handle(net::Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(std::string(kLayoutCache)), 0));
        REQUIRE(empty.handle(net::Opcode::SMSG_UPDATE_ACCOUNT_DATA, updatePacket(kLayoutCache), 10));
        CHECK_FALSE(fresh.reupload);
        CHECK(fresh.file.empty());
    }
    CHECK(isTouchLayoutText(mine));
    CHECK_FALSE(isTouchLayoutText(kLayoutCache));
    CHECK_FALSE(isTouchLayoutText(""));
}
