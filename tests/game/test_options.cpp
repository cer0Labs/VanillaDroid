// Phase 15 (options): what the interface options send and change through game::Gameplay, end to end with a capturing
// packet sink: Show Helm / Show Cloak (CMSG_TOGGLE_HELM / _CLOAK, as WorldSession::HandleShowingHelmOpcode reads
// them: no body), the extra action bars (CMSG_SET_ACTIONBAR_TOGGLES, one byte, HandleSetActionBarTogglesOpcode),
// Block Trades (CMSG_BUSY_TRADE) and Auto Self Cast off (a helpful spell without a fitting target is refused). And
// /macro.
#include "game/ChatCommands.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/SpellTargeting.h"
#include "net/ByteBuffer.h"
#include "net/ExchangeMessages.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kFriend = 0x22;
constexpr u32 kHeal = 2050;   // Lesser Heal: SPELL_EFFECT_HEAL on TARGET_UNIT_FRIEND
constexpr u32 kRenew = 139;   // the same target rule, another spell

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

Bytes valuesUpdate(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid); // UPDATETYPE_VALUES
    writeValues(w, fields);
    return out;
}

game::SpellInfo heal(u32 id) {
    game::SpellInfo s;
    s.id = id;
    s.effects[0] = 10;          // SPELL_EFFECT_HEAL
    s.implicitTargetA[0] = 21;  // TARGET_UNIT_FRIEND
    s.maxRange = 40.0f;
    return s;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<game::SpellInfo> spells{heal(kHeal), heal(kRenew)};
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
        game::SpellSource source;
        source.context = this;
        source.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
            for (const game::SpellInfo& info : static_cast<const Harness*>(context)->spells) {
                if (info.id == id) {
                    out = info;
                    return true;
                }
            }
            return false;
        };
        play.setSpellSource(source);
        play.enterWorld(kPlayer, 0, true, now);
        apply(created(kPlayer, game::ObjectType::Player,
                      {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 20}, {uf::PLAYER_FLAGS, 0}}));
        apply(created(kFriend, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 20}}));
        sent.clear();
        events();
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
        play.update(now);
    }
    std::vector<Bytes> take(Opcode opcode) {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        sent.clear();
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

Bytes castBody(u32 spellId, u64 unit) {
    Bytes out;
    net::SpellCastTargets t;
    if (unit != 0) {
        t.mask = net::kTargetUnit;
        t.unit = unit;
    }
    net::writeCastSpell(out, spellId, t);
    return out;
}

} // namespace

TEST_CASE("Options: Show Helm / Show Cloak toggle PLAYER_FLAGS only when the wish differs") {
    Harness h;
    CHECK(h.play.showingHelm());
    CHECK(h.play.showingCloak());
    h.play.showHelm(true); // already shown: nothing
    h.play.showCloak(true);
    CHECK(h.sent.empty());

    h.play.showHelm(false);
    std::vector<Bytes> helm = h.take(Opcode::CMSG_TOGGLE_HELM);
    REQUIRE(helm.size() == 1);
    CHECK(helm[0].empty());
    // The server's answer: PLAYER_FLAGS_HIDE_HELM (0x400). Asking again for it hidden sends nothing.
    h.apply(valuesUpdate(kPlayer, {{uf::PLAYER_FLAGS, 0x400}}));
    CHECK_FALSE(h.play.showingHelm());
    CHECK(h.play.showingCloak());
    h.play.showHelm(false);
    CHECK(h.sent.empty());

    h.play.showCloak(false);
    std::vector<Bytes> cloak = h.take(Opcode::CMSG_TOGGLE_CLOAK);
    REQUIRE(cloak.size() == 1);
    CHECK(cloak[0].empty());
    h.apply(valuesUpdate(kPlayer, {{uf::PLAYER_FLAGS, 0xC00}}));
    CHECK_FALSE(h.play.showingCloak());
    h.play.showHelm(true);
    CHECK(h.take(Opcode::CMSG_TOGGLE_HELM).size() == 1);
}

TEST_CASE("Options: the extra action bars are PLAYER_FIELD_BYTES byte 2, set with one byte") {
    Harness h;
    CHECK(h.play.actionBarToggles() == 0);
    h.apply(valuesUpdate(kPlayer, {{uf::PLAYER_FIELD_BYTES, 0x05u << 16}})); // bottom left + right bars
    CHECK(h.play.actionBarToggles() == 0x05);
    h.play.setActionBarToggles(0x05); // what the server holds: nothing
    CHECK(h.sent.empty());
    h.play.setActionBarToggles(0x0F);
    std::vector<Bytes> bars = h.take(Opcode::CMSG_SET_ACTIONBAR_TOGGLES);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0] == Bytes{0x0F});
    // Only bits 0..3 go (ALWAYS_SHOW_MULTIBARS stays on the client).
    h.play.setActionBarToggles(0x13);
    bars = h.take(Opcode::CMSG_SET_ACTIONBAR_TOGGLES);
    REQUIRE(bars.size() == 1);
    CHECK(bars[0] == Bytes{0x03});
}

TEST_CASE("Options: Block Trades answers a trade request with CMSG_BUSY_TRADE") {
    Harness h;
    Bytes begin;
    net::ByteWriter(begin).u32v(net::kTradeBegin).u64v(kFriend);

    h.play.exchange().trade().blockRequests = true;
    REQUIRE(h.play.handle(Opcode::SMSG_TRADE_STATUS, begin, h.now) == game::Gameplay::Handled::Yes);
    const std::vector<Bytes> busy = h.take(Opcode::CMSG_BUSY_TRADE);
    REQUIRE(busy.size() == 1);
    CHECK(busy[0].empty());
    const std::vector<game::GameEvent> events = h.events();
    const auto blocked = std::find_if(events.begin(), events.end(),
                                      [](const game::GameEvent& e) { return e.kind == GameEventKind::TradeBlocked; });
    REQUIRE(blocked != events.end());
    CHECK(blocked->guid == kFriend);
    CHECK(h.play.exchange().trade().phase() == game::TradePhase::None);

    // Off: the request opens the trade as before.
    h.play.exchange().trade().blockRequests = false;
    REQUIRE(h.play.handle(Opcode::SMSG_TRADE_STATUS, begin, h.now) == game::Gameplay::Handled::Yes);
    CHECK(h.take(Opcode::CMSG_BUSY_TRADE).empty());
    CHECK(h.play.exchange().trade().phase() == game::TradePhase::Requested);
}

TEST_CASE("Options: Auto Self Cast off refuses a helpful spell without a fitting target") {
    Harness h;
    const auto refusal = [&]() -> i32 {
        const std::vector<game::GameEvent> events = h.events();
        const auto failed = std::find_if(events.begin(), events.end(),
                                         [](const game::GameEvent& e) { return e.kind == GameEventKind::CastFailed; });
        return failed != events.end() && failed->id == kHeal ? failed->value[0] : -1;
    };
    // On (the default here, a deliberate change; 1.12's is off): no target, the heal lands on the player (no unit in
    // the targets).
    CHECK(h.play.autoSelfCast);
    CHECK(h.play.spellTarget(kHeal) == kPlayer);
    REQUIRE(h.play.castSpell(kHeal, h.play.spellTarget(kHeal)));
    std::vector<Bytes> casts = h.take(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);
    CHECK(casts[0] == castBody(kHeal, 0));
    h.events();

    // Off: nothing fitting, nothing sent and nothing waiting for a unit (no targeting window): "No target".
    h.play.autoSelfCast = false;
    CHECK(h.play.spellTarget(kHeal) == 0);
    CHECK_FALSE(h.play.castSpell(kHeal, h.play.spellTarget(kHeal)));
    CHECK(h.sent.empty());
    CHECK_FALSE(h.play.targeting().active());
    CHECK(refusal() == 9); // SPELL_FAILED_BAD_IMPLICIT_TARGETS

    // A target that is not friendly (no faction tables here: neutral) does not take it either: "Invalid target".
    h.play.setTarget(kFriend);
    h.sent.clear();
    CHECK(h.play.spellTarget(kHeal) == 0);
    CHECK_FALSE(h.play.castSpell(kHeal, h.play.spellTarget(kHeal)));
    CHECK(h.sent.empty());
    CHECK(refusal() == 10); // SPELL_FAILED_BAD_TARGETS
    CHECK(h.play.target() == kFriend);

    // The player selected: on the player, as with the option on.
    h.play.setTarget(kPlayer);
    h.sent.clear();
    CHECK(h.play.spellTarget(kHeal) == kPlayer);
    REQUIRE(h.play.castSpell(kHeal, h.play.spellTarget(kHeal)));
    casts = h.take(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);
    CHECK(casts[0] == castBody(kHeal, 0));
}

TEST_CASE("Options: /macro opens the macro window, /macrohelp prints its help") {
    using game::ChatCommandKind;
    CHECK(game::parseChatInput("/macro", net::ChatType::Say, {}, 0).kind == ChatCommandKind::Macro);
    CHECK(game::parseChatInput("/m", net::ChatType::Say, {}, 0).kind == ChatCommandKind::Macro);
    CHECK(game::parseChatInput("/MACROS", net::ChatType::Say, {}, 0).kind == ChatCommandKind::Macro);
    CHECK(game::parseChatInput("/macrohelp", net::ChatType::Say, {}, 0).kind == ChatCommandKind::MacroHelp);
}
