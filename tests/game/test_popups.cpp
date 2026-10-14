// Parity (popups): what is behind the client's StaticPopups in the game layer, end to end through
// game::Gameplay with a capturing packet sink: the logout state machine (CMSG_LOGOUT_REQUEST,
// SMSG_LOGOUT_RESPONSE refused / at once / counting down, CMSG_LOGOUT_CANCEL, SMSG_LOGOUT_CANCEL_ACK),
// summons (SMSG_SUMMON_REQUEST, CMSG_SUMMON_RESPONSE, expiry), the DEATH popup's self-resurrection
// (PLAYER_SELF_RES_SPELL, CMSG_SELF_RES), the resurrection's corpse delay flag and the soulbinding questions
// (game/BindConfirm). Server packets are written as vmangos-core 4641790 writes them; client packets are
// checked byte for byte.
#include "game/BindConfirm.h"
#include "game/ChatCommands.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/ItemActions.h"
#include "game/ItemDefs.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/PopupMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::BindAsk;
using game::GameEventKind;
using game::ItemAction;
using game::ItemPlace;
using game::ItemPos;
using game::LogoutPhase;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kSummoner = 0x22;
constexpr u64 kHelm = 0x4000000000000a01ull;   // bind on equip, in the backpack
constexpr u64 kRing = 0x4000000000000a02ull;   // bind on equip, already soulbound
constexpr u64 kPotion = 0x4000000000000a03ull; // bind on use
constexpr u64 kWorn = 0x4000000000000a04ull;   // worn on the head
constexpr u64 kBag = 0x4000000000000a05ull;    // a bind on equip bag in the backpack

constexpr u32 kHelmEntry = 10001;
constexpr u32 kRingEntry = 10002;
constexpr u32 kPotionEntry = 10003;
constexpr u32 kWornEntry = 10004;
constexpr u32 kBagEntry = 10005;
constexpr u32 kPickupEntry = 10006; // bind on pickup (loot)
constexpr u32 kQuestEntry = 10007;  // a quest item (never asked)

constexpr u8 kBackpackFirst = 23; // INVENTORY_SLOT_ITEM_START
constexpr u8 kHeadSlot = 0;

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    const u32 words = maxIndex / 32 + 1;
    std::vector<u32> mask(words, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

Bytes itemObject(u64 guid, u32 entry, u32 flags = 0) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry},
                    {uf::ITEM_FIELD_OWNER, low(kPlayer)},
                    {uf::ITEM_FIELD_STACK_COUNT, 1},
                    {uf::ITEM_FIELD_FLAGS, flags}});
}

void slot(Fields& f, u8 slotIndex, u64 guid) {
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2), low(guid)});
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2 + 1), high(guid)});
}

// HandleItemQuerySingleOpcode's layout (ItemQuerySingleResponse): only what the binding rules read is set.
Bytes itemQuery(u32 entry, u32 inventoryType, u32 bonding, u32 containerSlots = 0) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(4).u32v(1).cstring("Item").cstring("").cstring("").cstring("");
    w.u32v(1).u32v(2).u32v(0).u32v(0).u32v(0).u32v(inventoryType); // display, quality, flags, prices
    w.u32v(0xFFFFFFFF).u32v(0xFFFFFFFF).u32v(10).u32v(0);          // classes, races, item level, level
    for (int i = 0; i < 8; ++i) {
        w.u32v(0); // skill, rank, spell, honor rank, city rank, faction, rank, max count
    }
    w.u32v(1).u32v(containerSlots);
    for (int i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (int i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (int i = 0; i < 7; ++i) {
        w.u32v(0); // armor, six resistances
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (int i = 0; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0xFFFFFFFF).u32v(0).u32v(0xFFFFFFFF);
    }
    w.u32v(bonding).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
    u32 now = 1000;
    u32 cursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
        cursor = play.events().end();
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
        play.update(now);
        play.inventory().refresh(now, true);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    u32 count(Opcode opcode) const {
        return static_cast<u32>(
            std::count_if(sent.begin(), sent.end(), [&](const Sent& s) { return s.opcode == opcode; }));
    }
    const Bytes* last(Opcode opcode) const {
        const Bytes* out = nullptr;
        for (const Sent& s : sent) {
            out = s.opcode == opcode ? &s.body : out;
        }
        return out;
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
};

Bytes logoutResponse(u32 reason, u8 instant) {
    Bytes out;
    net::ByteWriter(out).u32v(reason).u8v(instant);
    return out;
}

} // namespace

TEST_CASE("Popups: the logout state machine (CAMP / QUIT)") {
    Harness h;
    // Logout(): CMSG_LOGOUT_REQUEST, empty.
    REQUIRE(h.play.logout().request(false, h.now));
    REQUIRE(h.count(Opcode::CMSG_LOGOUT_REQUEST) == 1);
    CHECK(h.last(Opcode::CMSG_LOGOUT_REQUEST)->empty());
    CHECK(h.play.logout().phase() == LogoutPhase::Requested);
    CHECK_FALSE(h.play.logout().request(false, h.now)); // one at a time
    CHECK(h.count(Opcode::CMSG_LOGOUT_REQUEST) == 1);

    SUBCASE("refused: in combat") {
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, logoutResponse(net::kLogoutRefusedCombat, 0)) ==
                game::Gameplay::Handled::Yes);
        const std::vector<game::GameEvent> e = h.events();
        REQUIRE(e.size() == 1);
        CHECK(e[0].kind == GameEventKind::LogoutRefused);
        CHECK(e[0].value[0] == 1);
        CHECK(h.play.logout().phase() == LogoutPhase::None);
        CHECK(h.play.logout().request(false, h.now)); // may ask again
    }
    SUBCASE("at once: resting, a taxi, a GM") {
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, logoutResponse(0, 1)) == game::Gameplay::Handled::Yes);
        const std::vector<game::GameEvent> e = h.events();
        REQUIRE(e.size() == 1);
        CHECK(e[0].kind == GameEventKind::LogoutInstant);
        CHECK(h.play.logout().phase() == LogoutPhase::Instant);
        CHECK(h.play.logout().pending());
        CHECK_FALSE(h.play.logout().cancel()); // nothing to cancel
        CHECK(h.count(Opcode::CMSG_LOGOUT_CANCEL) == 0);
    }
    SUBCASE("the countdown, then cancelled") {
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, logoutResponse(0, 0)) == game::Gameplay::Handled::Yes);
        std::vector<game::GameEvent> e = h.events();
        REQUIRE(e.size() == 1);
        CHECK(e[0].kind == GameEventKind::LogoutStarted);
        CHECK(e[0].value[0] == 0); // CAMP, not QUIT
        CHECK(h.play.logout().phase() == LogoutPhase::Countdown);
        CHECK(h.play.logout().remainingMs(h.now) == game::Logout::kCountdownMs);
        CHECK(h.play.logout().remainingMs(h.now + 15000) == 5000);
        CHECK(h.play.logout().remainingMs(h.now + 25000) == 0);
        // CancelLogout: CMSG_LOGOUT_CANCEL, empty; the ack ends it.
        REQUIRE(h.play.logout().cancel());
        REQUIRE(h.count(Opcode::CMSG_LOGOUT_CANCEL) == 1);
        CHECK(h.last(Opcode::CMSG_LOGOUT_CANCEL)->empty());
        CHECK(h.play.logout().phase() == LogoutPhase::Cancelling);
        CHECK_FALSE(h.play.logout().cancel()); // once
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_CANCEL_ACK, {}) == game::Gameplay::Handled::Yes);
        e = h.events();
        REQUIRE(e.size() == 1);
        CHECK(e[0].kind == GameEventKind::LogoutCancelled);
        CHECK(h.play.logout().phase() == LogoutPhase::None);
    }
    SUBCASE("cancelled before the answer came") {
        REQUIRE(h.play.logout().cancel());
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, logoutResponse(0, 0)) == game::Gameplay::Handled::Yes);
        CHECK(h.events().empty()); // no countdown shown
        CHECK(h.play.logout().phase() == LogoutPhase::Cancelling);
        REQUIRE(h.packet(Opcode::SMSG_LOGOUT_CANCEL_ACK, {}) == game::Gameplay::Handled::Yes);
        const std::vector<game::GameEvent> e = h.events();
        REQUIRE(e.size() == 1);
        CHECK(e[0].kind == GameEventKind::LogoutCancelled);
    }
    SUBCASE("malformed answers change nothing") {
        CHECK(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, {0, 0, 0, 0}) == game::Gameplay::Handled::Malformed);
        CHECK(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, {}) == game::Gameplay::Handled::Malformed);
        CHECK(h.events().empty());
        CHECK(h.play.logout().phase() == LogoutPhase::Requested);
    }
    SUBCASE("leaving the world forgets it") {
        h.play.leaveWorld();
        CHECK(h.play.logout().phase() == LogoutPhase::None);
        CHECK_FALSE(h.play.logout().request(false, h.now)); // not in the world
    }
}

TEST_CASE("Popups: Quit() is remembered through the countdown") {
    Harness h;
    REQUIRE(h.play.logout().request(true, h.now));
    CHECK(h.play.logout().quitting());
    REQUIRE(h.packet(Opcode::SMSG_LOGOUT_RESPONSE, logoutResponse(0, 0)) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == GameEventKind::LogoutStarted);
    CHECK(e[0].value[0] == 1); // PLAYER_QUITING: the QUIT popup
    h.play.leaveWorld();       // SMSG_LOGOUT_COMPLETE (app/GameSession)
    CHECK(h.play.logout().quitting());
    h.play.enterWorld(kPlayer, 0, true, h.now); // a new login starts over
    CHECK_FALSE(h.play.logout().quitting());
}

TEST_CASE("Popups: a summon is accepted, or expires") {
    Harness h;
    Bytes request;
    net::ByteWriter(request).u64v(kSummoner).u32v(12).u32v(120000); // Elwynn Forest, two minutes
    REQUIRE(h.packet(Opcode::SMSG_SUMMON_REQUEST, request) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == GameEventKind::SummonRequested);
    CHECK(e[0].guid == kSummoner);
    CHECK(e[0].id == 12);
    CHECK(e[0].value[0] == 120000);
    CHECK(h.play.summon().summoner(h.now) == kSummoner);
    CHECK(h.play.summon().zone() == 12);
    CHECK(h.play.summon().remainingMs(h.now + 20000) == 100000);

    SUBCASE("accepted: CMSG_SUMMON_RESPONSE with the summoner") {
        REQUIRE(h.play.summon().accept(h.now + 1000));
        const Bytes* body = h.last(Opcode::CMSG_SUMMON_RESPONSE);
        REQUIRE(body);
        CHECK(*body == Bytes{0x22, 0, 0, 0, 0, 0, 0, 0});
        CHECK(h.play.summon().summoner(h.now) == 0);
        CHECK_FALSE(h.play.summon().accept(h.now)); // once
    }
    SUBCASE("expired: nothing to accept") {
        CHECK(h.play.summon().summoner(h.now + 120000) == 0);
        CHECK_FALSE(h.play.summon().accept(h.now + 120001));
        CHECK(h.count(Opcode::CMSG_SUMMON_RESPONSE) == 0);
    }
    SUBCASE("declined: forgotten, never answered") {
        h.play.summon().decline();
        CHECK(h.play.summon().summoner(h.now) == 0);
        CHECK(h.count(Opcode::CMSG_SUMMON_RESPONSE) == 0);
    }
    CHECK(h.packet(Opcode::SMSG_SUMMON_REQUEST, Bytes(15, 0)) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Popups: the DEATH popup's self-resurrection and the resurrection's delay") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_LEVEL, 30}}));
    CHECK(h.play.death().selfResSpell == 0);
    h.play.selfResurrect(); // alive: nothing
    CHECK(h.count(Opcode::CMSG_SELF_RES) == 0);
    // Killed with a soulstone on (Unit::Kill: PLAYER_SELF_RES_SPELL), the release timer flag
    // (PLAYER_FIELD_BYTE_RELEASE_TIMER) in PLAYER_FIELD_BYTES' first byte.
    h.apply(values(
        kPlayer,
        {{uf::UNIT_FIELD_HEALTH, 0}, {uf::PLAYER_SELF_RES_SPELL, 20707}, {uf::PLAYER_FIELD_BYTES, 0x08}}));
    CHECK(h.play.death().dead);
    CHECK(h.play.death().releaseTimer);
    CHECK(h.play.death().selfResSpell == 20707);
    // UseSoulstone: CMSG_SELF_RES, empty.
    h.play.selfResurrect();
    REQUIRE(h.count(Opcode::CMSG_SELF_RES) == 1);
    CHECK(h.last(Opcode::CMSG_SELF_RES)->empty());
    // The server clears the field once cast.
    h.apply(values(kPlayer, {{uf::PLAYER_SELF_RES_SPELL, 0}}));
    CHECK(h.play.death().selfResSpell == 0);

    // SMSG_RESURRECT_REQUEST: guid, name length + name, sickness, delayed.
    Bytes resurrect;
    net::ByteWriter(resurrect).u64v(0x33).u32v(1).u8v(0).u8v(0).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_RESURRECT_REQUEST, resurrect) == game::Gameplay::Handled::Yes);
    CHECK(h.play.death().resurrector == 0x33);
    CHECK_FALSE(h.play.death().resurrectSickness);
    CHECK(h.play.death().resurrectDelayed);
}

TEST_CASE("Popups: which actions ask before binding (LOOT_BIND, EQUIP_BIND, AUTOEQUIP_BIND, USE_BIND)") {
    Harness h;
    h.apply(itemObject(kHelm, kHelmEntry));
    h.apply(itemObject(kRing, kRingEntry, game::kItemDynFlagBound));
    h.apply(itemObject(kPotion, kPotionEntry));
    h.apply(itemObject(kWorn, kWornEntry));
    h.apply(created(kBag, game::ObjectType::Container,
                    {{uf::OBJECT_FIELD_ENTRY, kBagEntry},
                     {uf::ITEM_FIELD_STACK_COUNT, 1},
                     {uf::CONTAINER_FIELD_NUM_SLOTS, 6}}));
    Fields player = {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_LEVEL, 30}};
    slot(player, kHeadSlot, kWorn);
    slot(player, kBackpackFirst, kHelm);
    slot(player, kBackpackFirst + 1, kRing);
    slot(player, kBackpackFirst + 2, kPotion);
    slot(player, kBackpackFirst + 3, kBag);
    h.apply(created(kPlayer, game::ObjectType::Player, player));
    const ItemPlace helm = ItemPlace::slot({255, kBackpackFirst});
    const ItemPlace ring = ItemPlace::slot({255, kBackpackFirst + 1});
    const ItemPlace potion = ItemPlace::slot({255, kBackpackFirst + 2});
    const ItemPlace bag = ItemPlace::slot({255, kBackpackFirst + 3});
    const ItemPlace head = ItemPlace::slot({255, kHeadSlot});

    // Templates not known yet: nothing is asked (the server binds anyway).
    CHECK(game::itemActionBindAsk(h.play, helm, ItemAction::Equip) == BindAsk::None);
    CHECK(game::lootBindAsk(h.play, kPickupEntry) == BindAsk::None);

    // ItemBondingType: 1 pickup, 2 equip, 3 use, 4 quest. Inventory types: 1 head, 11 finger, 0 none, 18 bag.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kHelmEntry, 1, game::kBindOnEquip)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                     itemQuery(kRingEntry, 11, game::kBindOnEquip)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kPotionEntry, 0, game::kBindOnUse)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kWornEntry, 1, game::kBindNone)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                     itemQuery(kBagEntry, 18, game::kBindOnEquip, 6)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                     itemQuery(kPickupEntry, 1, game::kBindOnPickup)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kQuestEntry, 0, game::kBindQuest)) ==
            game::Gameplay::Handled::Yes);

    // Loot: only bind on pickup.
    CHECK(game::lootBindAsk(h.play, kPickupEntry) == BindAsk::Loot);
    CHECK(game::lootBindAsk(h.play, kQuestEntry) == BindAsk::None);
    CHECK(game::lootBindAsk(h.play, kHelmEntry) == BindAsk::None);
    // Equip from the tooltip (AutoEquipContainerItem): an unbound bind-on-equip item.
    CHECK(game::itemActionBindAsk(h.play, helm, ItemAction::Equip) == BindAsk::AutoEquip);
    CHECK(game::itemActionBindAsk(h.play, ring, ItemAction::Equip) == BindAsk::None); // soulbound already
    CHECK(game::itemActionBindAsk(h.play, helm, ItemAction::Sell) == BindAsk::None);
    // Use: bind on use.
    CHECK(game::itemActionBindAsk(h.play, potion, ItemAction::Use) == BindAsk::Use);
    CHECK(game::useItemBindAsk(h.play, kPotionEntry) == BindAsk::Use);
    CHECK(game::useItemBindAsk(h.play, kHelmEntry) == BindAsk::None);
    // Drops: onto an equipment slot (EQUIP_BIND), the character window (AUTOEQUIP_BIND), a bag into a bag
    // slot, a worn item onto a bag cell holding one that would take its place.
    CHECK(game::dropBindAsk(h.play, helm, head) == BindAsk::Equip);
    CHECK(game::dropBindAsk(h.play, helm, ItemPlace::of(ItemPlace::Kind::Equip)) == BindAsk::AutoEquip);
    CHECK(game::dropBindAsk(h.play, ring, ItemPlace::slot({255, 10})) == BindAsk::None);
    CHECK(game::dropBindAsk(h.play, bag, ItemPlace::slot({255, 19})) == BindAsk::Equip);
    CHECK(game::dropBindAsk(h.play, helm, ItemPlace::slot({255, 19})) == BindAsk::None); // into that bag
    CHECK(game::dropBindAsk(h.play, head, helm) == BindAsk::Equip);
    CHECK(game::dropBindAsk(h.play, head, potion) == BindAsk::None); // a potion is not worn
    CHECK(game::dropBindAsk(h.play, helm, ItemPlace::slot({255, kBackpackFirst + 5})) == BindAsk::None);
    CHECK(game::dropBindAsk(h.play, helm, ItemPlace::of(ItemPlace::Kind::Sell)) == BindAsk::None);
    // Once bound, never again.
    h.apply(values(kHelm, {{uf::ITEM_FIELD_FLAGS, game::kItemDynFlagBound}}));
    CHECK(game::dropBindAsk(h.play, helm, head) == BindAsk::None);
    CHECK(game::itemActionBindAsk(h.play, helm, ItemAction::Equip) == BindAsk::None);
}

TEST_CASE("Popups: /logout, /camp, /quit and /exit") {
    using K = game::ChatCommandKind;
    CHECK(game::parseChatInput("/logout", net::ChatType::Say).kind == K::Logout);
    CHECK(game::parseChatInput("/camp", net::ChatType::Say).kind == K::Logout);
    CHECK(game::parseChatInput("/quit", net::ChatType::Say).kind == K::Quit);
    CHECK(game::parseChatInput("/EXIT", net::ChatType::Say).kind == K::Quit);
}
