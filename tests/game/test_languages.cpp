// Phase 18 (extras): B4, the other languages garbled as the 1.12 client garbles them (game/Languages,
// WoW.exe 1.12.1 0x49B560 / 0x64AF90 / 0x5EC720 / 0x49A970) with sample word lists and with the real
// LanguageWords.dbc (VD_PACKS_DIR: db.vdpk; skipped without it), through game::Gameplay with synthetic
// SMSG_MESSAGECHAT packets, the chat line's "[Orcish] " and the chat bubble; and the small packets the client
// used to ignore (net/ExtrasMessages) byte for byte as vmangos-core 4641790 writes them.
#include "TestData.h"
#include "app/ChatBubbles.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Languages.h"
#include "game/SocialText.h"
#include "net/ByteBuffer.h"
#include "net/ExtrasMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::FxEventKind;
using game::LanguageData;
using net::ChatType;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u32 kOrcish = 1;
constexpr u32 kCommon = 7;
constexpr u32 kLanguageCommon = 668; // spells: Language Common, Language Orcish
constexpr u32 kLanguageOrcish = 669;
constexpr u32 kSkillCommon = 98; // skill lines
constexpr u32 kSkillOrcish = 109;
constexpr u64 kPlayer = 0x11;
constexpr u64 kOrc = 0x12;
constexpr u64 kGrunt = 0xF130000000000456ull;

// A small LanguageData: some Orcish and Common words in LanguageWords' order, the two languages' spells.
struct SampleLanguages {
    LanguageData data;
    SampleLanguages() {
        static const std::pair<u32, std::string_view> kNames[] = {{kOrcish, "Orcish"}, {kCommon, "Common"}};
        static const LanguageData::Word kWords[] = {
            {kOrcish, "A"},     {kOrcish, "N"},   {kOrcish, "Ha"},  {kOrcish, "Ko"},   {kOrcish, "No"},
            {kOrcish, "Lok"},   {kOrcish, "Tar"}, {kOrcish, "Kaz"}, {kOrcish, "Rega"}, {kOrcish, "Nogu"},
            {kOrcish, "Kazum"}, {kOrcish, ""},    {kCommon, "Lo"},  {kCommon, "Ras"},  {kCommon, "Majis"}};
        static const LanguageData::Teacher kTeachers[] = {{kLanguageOrcish, kOrcish, kSkillOrcish, 0, 0},
                                                          {kLanguageCommon, kCommon, kSkillCommon, 0, 0}};
        data.set(kNames, kWords, kTeachers);
    }
};

std::string scramble(const LanguageData& data, u32 language, u32 skill, std::string_view text,
                     game::ScrambleRules rules = {}) {
    std::string out;
    game::scrambleLanguage(data, language, skill, text, out, rules);
    return out;
}

// The real tables, loaded once (nullptr without the packs).
const LanguageData* realLanguages() {
    static pack::PackReader db;
    static LanguageData data;
    static int state = 0; // 0 not tried, 1 ready, 2 missing
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
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

// SMSG_MESSAGECHAT as ChatHandler::BuildChatPacket writes a player's line (say, yell, party, emote...).
// Say, party and yell carry the sender twice.
Bytes playerLine(ChatType type, u32 language, u64 sender, std::string_view text) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(static_cast<u8>(type)).u32v(language).u64v(sender);
    if (type == ChatType::Say || type == ChatType::Party || type == ChatType::Yell) {
        w.u64v(sender);
    }
    w.u32v(static_cast<u32>(text.size() + 1)).cstring(text).u8v(0);
    return out;
}

// ... and a creature's yell (Creature::MonsterYell: its GUID, its name, whom it is addressed to).
Bytes monsterYell(u32 language, u64 creature, std::string_view name, std::string_view text) {
    Bytes out;
    net::ByteWriter(out)
        .u8v(static_cast<u8>(ChatType::MonsterYell))
        .u32v(language)
        .u64v(creature)
        .u32v(static_cast<u32>(name.size() + 1))
        .cstring(name)
        .u64v(0)
        .u32v(static_cast<u32>(text.size() + 1))
        .cstring(text)
        .u8v(0);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// A human warrior who speaks Common (Language Common known, its skill at 300/300), its packets recorded.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    SampleLanguages languages;
    std::vector<Sent> sent;
    u32 now = 1000;
    u32 chatCursor = 0;
    u32 fxCursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.setLanguageData(&languages.data);
        play.enterWorld(kPlayer, 0, true, now);
        apply(created(kPlayer, game::ObjectType::Player,
                      {{uf::UNIT_FIELD_HEALTH, 100},
                       {uf::UNIT_FIELD_BYTES_0, 1 | (1u << 8)}, // a human warrior
                       {uf::PLAYER_SKILL_INFO_1_1, kSkillCommon},
                       {static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + 1), 300 | (300u << 16)}}));
        Bytes spells;
        net::ByteWriter(spells).u8v(0).u16v(1).u16v(kLanguageCommon).u16v(0).u16v(0);
        REQUIRE(packet(Opcode::SMSG_INITIAL_SPELLS, spells) == game::Gameplay::Handled::Yes);
        chatCursor = play.chat().end();
        fxCursor = play.fx().end();
    }
    void apply(const Bytes& update) { REQUIRE(store.applyUpdateObject(update, now)); }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    // The next chat line (REQUIREd).
    const game::ChatLine& heard(const Bytes& message) {
        REQUIRE(packet(Opcode::SMSG_MESSAGECHAT, message) == game::Gameplay::Handled::Yes);
        const game::ChatLine* line = play.chat().next(chatCursor);
        REQUIRE(line != nullptr);
        return *line;
    }
    std::vector<game::FxEvent> fx(FxEventKind kind) {
        std::vector<game::FxEvent> out;
        while (const game::FxEvent* e = play.fx().next(fxCursor)) {
            if (e->kind == kind) {
                out.push_back(*e);
            }
        }
        return out;
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE("Languages: SStrHash and the scrambling of WoW.exe 0x49B560") {
    // SStrHash(word, 0, 0): case-insensitive (the reference values are the disassembly's algorithm run by
    // hand).
    CHECK(game::languageHash("Hello") == 0x1895E18Fu);
    CHECK(game::languageHash("hello") == 0x1895E18Fu);
    CHECK(game::languageHash("HELLO") == 0x1895E18Fu);
    CHECK(game::languageHash("a") == 0x39AE09CFu);
    CHECK(game::languageHash("For the Horde") == 0x5FFAC072u);
    CHECK(game::languageHash("Lok'tar") == 0x59CC8EFEu);
    CHECK(game::languageHash("") == 0x7FED7FEDu);
    CHECK(game::languageHash("a/b") == game::languageHash("A\\B"));

    const SampleLanguages sample;
    const LanguageData& data = sample.data;
    CHECK(data.name(kOrcish) == "Orcish");
    CHECK(data.name(0).empty());
    REQUIRE(data.words(kOrcish, 3).size() == 3);
    CHECK(data.words(kOrcish, 3)[0] == "Lok"); // the table's order within a length
    CHECK(data.words(kOrcish, 6).empty());
    u32 taught = 0;
    CHECK(data.teaches(kLanguageOrcish, taught));
    CHECK(taught == kOrcish);
    CHECK_FALSE(data.teaches(133, taught));
    CHECK(data.skillLine(kLanguageOrcish, 2, 1) == kSkillOrcish);

    // The chat's rules: each word replaced by one of its length (cased as it), one space for any run between
    // words (the punctuation goes, a trailing one leaves a space).
    CHECK(scramble(data, kOrcish, 0, "Hello, how are you?") == "Kazum kaz kaz kaz ");
    CHECK(scramble(data, kOrcish, 0, "For the Horde!") == "Tar kaz Kazum ");
    CHECK(scramble(data, kOrcish, 0, "WHAT is this") == "REGA ha rega");
    // Hyphens split words; a word longer than any of the language's takes the longest under it.
    CHECK(scramble(data, kOrcish, 0, "Throm-Ka, friend.") == "Kazum Ko kazum ");
    CHECK(scramble(data, kCommon, 0, "ab") == "lo");
    CHECK(scramble(data, kCommon, 0, "Hello world") == "Majis majis");
    // UTF-8: "ñ" is two bytes of the word; the case follows ASCII only.
    CHECK(scramble(data, kOrcish, 0,
                   "Se\xC3\xB1or \xC3\x91"
                   "and\xC3\xBA") == "Kazum kazum");
    // A word whose hash % 300 is under the skill stays (Hello 91, how 98, are 242, you 23).
    CHECK(scramble(data, kOrcish, 150, "Hello, how are you?") == "Hello how kaz you ");
    CHECK(scramble(data, kOrcish, 299, "Hello, how are you?") == "Hello how are you ");
    // 300, or Universal: as it is.
    CHECK(scramble(data, kOrcish, 300, "Hello, how are you?") == "Hello, how are you?");
    CHECK(scramble(data, 0, 0, "Hello, how are you?") == "Hello, how are you?");
    // The gossip texts' rules keep what lies between words (0x4E22AB), and "<...>" when asked (0x4E3856).
    CHECK(scramble(data, kOrcish, 0, "Hello, how are you?", {true, false}) == "Kazum, kaz kaz kaz?");
    CHECK(scramble(data, kOrcish, 0, "<Grunt> Lok'tar ogar!", {true, true}) == "<Grunt> Kazum nogu!");
    // A language without words of a word's length nor shorter drops the word.
    CHECK(scramble(data, kCommon, 0, "a b") == " ");
    // The client's buffer.
    std::string out;
    game::scrambleLanguage(data, kOrcish, 0, "Hello, how are you?", out, {}, 7);
    CHECK(out == "Kazum k");
}

TEST_CASE("Languages: LanguageWords, Languages and the language spells of the 1.12 data") {
    const LanguageData* data = realLanguages();
    if (!data) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    CHECK(data->name(kOrcish) == "Orcish");
    CHECK(data->name(kCommon) == "Common");
    CHECK(data->name(33) == "Gutterspeak");
    CHECK(data->name(0).empty()); // no Universal row: no "[...]"
    CHECK(data->words(kOrcish, 3).size() == 12);
    CHECK(data->words(14, 9).size() == 2); // Troll's longest words
    CHECK(data->words(14, 10).empty());
    u32 language = 0;
    REQUIRE(data->teaches(kLanguageOrcish, language));
    CHECK(language == kOrcish);
    REQUIRE(data->teaches(17737, language)); // Language Gutterspeak
    CHECK(language == 33);
    CHECK(data->skillLine(kLanguageOrcish, 2, 1) == kSkillOrcish); // an orc warrior
    CHECK(data->skillLine(kLanguageCommon, 1, 1) == kSkillCommon); // a human warrior
    CHECK(data->skillLine(7340, 7, 4) == 313); // Language Gnomish: gnomes only (race mask 64)
    CHECK(data->skillLine(7340, 1, 4) == 0);
    // The same sentences as the reference implementation of 0x49B560 over LanguageWords.dbc.
    CHECK(scramble(*data, kOrcish, 0, "Hello, how are you?") == "Kazum kaz kaz ogg ");
    CHECK(scramble(*data, kCommon, 0, "Hello, how are you?") == "Majis ras ras bur ");
    CHECK(scramble(*data, kCommon, 0, "For the Horde!") == "Nud ras Majis ");
    CHECK(scramble(*data, kOrcish, 0, "For the Horde!") == "Kil mog Ro'th ");
    CHECK(scramble(*data, kOrcish, 150, "Lok'tar ogar, friend.") == "Lok'tar ogar friend ");
    CHECK(scramble(*data, 2, 0, "WTS [Linen Cloth] 5s each") == "ASH Terro Turus ri andu");
    CHECK(scramble(*data, kOrcish, 0, "Hello, how are you?", {true, false}) == "Kazum, kaz kaz ogg?");
    CHECK(scramble(*data, 3, 0, "<Name> says hi.", {true, true}) == "<Name> mani ta.");
    CHECK(scramble(*data, 14, 0, "Supercalifragilisticexpialidocious") == "Or'manley");
    CHECK(scramble(*data, 6, 0, "I") == "A");
    CHECK(scramble(*data, 33, 0, "x y z 1 2") == "y o e o e");
}

TEST_CASE("Languages: what the player reads of a line in another language (Gameplay, 0x49A970)") {
    Harness h;
    CHECK(h.play.languageSkill(kCommon) == 300);
    CHECK(h.play.languageSkill(kOrcish) == 0);

    // An orc's /say in Orcish: garbled, with the language before it.
    const game::ChatLine& orc = h.heard(playerLine(ChatType::Say, kOrcish, kOrc, "Hello, how are you?"));
    CHECK(orc.text == "Kazum kaz kaz kaz ");
    CHECK(orc.languageName == "Orcish");
    CHECK(orc.language == kOrcish);
    // The bubble over the orc shows the same garbled text.
    ChatBubbles bubbles;
    REQUIRE(bubbles.add(orc, h.now, {true, true}));
    CHECK(bubbles.of(kOrc)->view() == "Kazum kaz kaz kaz ");
    // In Common (the human's own language): as it is, no header.
    const game::ChatLine& own = h.heard(playerLine(ChatType::Say, kCommon, kOrc, "Hello, how are you?"));
    CHECK(own.text == "Hello, how are you?");
    CHECK(own.languageName.empty());
    // An emote in Orcish counts as Universal.
    const game::ChatLine& emote = h.heard(playerLine(ChatType::Emote, kOrcish, kOrc, "grins."));
    CHECK(emote.text == "grins.");
    CHECK(emote.languageName.empty());
    // A creature's yell in Orcish.
    const game::ChatLine& yell = h.heard(monsterYell(kOrcish, kGrunt, "Orgrimmar Grunt", "For the Horde!"));
    CHECK(yell.text == "Tar kaz Kazum ");
    CHECK(yell.languageName == "Orcish");
    CHECK(h.play.senderName(yell) == "Orgrimmar Grunt");
    // A gossip text in Orcish (0x4E22AB): what lies between words stays; Common and Universal as they are.
    // The variant shown is the most probable one with text, its language with it.
    std::string heard;
    CHECK(h.play.gossipText(kOrcish, "Hello, how are you?", heard) == "Kazum, kaz kaz kaz?");
    CHECK(h.play.gossipText(kCommon, "Hello, how are you?", heard) == "Hello, how are you?");
    CHECK(h.play.gossipText(0, "Hello, how are you?", heard) == "Hello, how are you?");
    game::NpcText npcText;
    npcText.options[0] = {0.2f, "Well met.", "", kCommon, {}, {}};
    npcText.options[1] = {0.8f, "Lok'tar.", "", kOrcish, {}, {}};
    npcText.options[2] = {0.9f, "", "", 0, {}, {}};
    CHECK(npcText.text(0) == "Lok'tar.");
    CHECK(npcText.language() == kOrcish);
    // An addon's message (language -1) is CHAT_MSG_ADDON: no chat line.
    REQUIRE(h.packet(Opcode::SMSG_MESSAGECHAT, playerLine(ChatType::Party, 0xFFFFFFFFu, kOrc, "VER\t1.0")) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.play.chat().next(h.chatCursor) == nullptr);

    // Orcish learned at 150 (a GM's .learn 669 and .setskill 109 150): half the words come through.
    REQUIRE(h.packet(Opcode::SMSG_LEARNED_SPELL, [] {
        Bytes b;
        net::ByteWriter(b).u32v(kLanguageOrcish);
        return b;
    }()) == game::Gameplay::Handled::Yes);
    CHECK(h.play.languageSkill(kOrcish) == 0); // the spell, but no skill yet
    h.apply(values(kPlayer, {{static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + 3), kSkillOrcish},
                             {static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + 4), 150 | (300u << 16)}}));
    CHECK(h.play.languageSkill(kOrcish) == 150);
    CHECK(h.heard(playerLine(ChatType::Say, kOrcish, kOrc, "Hello, how are you?")).text ==
          "Hello how kaz you ");
    // Its bonuses (MAKE_SKILL_BONUS(temporary, permanent)): +20 permanent, -5 temporary.
    h.apply(values(kPlayer, {{static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + 5), 0xFFFBu | (20u << 16)}}));
    CHECK(h.play.languageSkill(kOrcish) == 165);

    // The languages it speaks now (GetLanguageByIndex: Languages.dbc's order) and the one the chat speaks in
    // (ChatMenu's Language submenu): Orcish, sent as such; the player's own by default.
    std::array<u32, 8> known{};
    REQUIRE(h.play.knownLanguages(known) == 2);
    CHECK(known[0] == kOrcish);
    CHECK(known[1] == kCommon);
    CHECK(h.play.defaultLanguage() == kCommon);
    const auto said = [&](std::string_view text) {
        Bytes body;
        net::ByteWriter(body).u32v(static_cast<u32>(ChatType::Say)).u32v(0).cstring(text);
        return body;
    };
    h.sent.clear();
    REQUIRE(h.play.sendChat(ChatType::Say, "hello"));
    Bytes inCommon = said("hello");
    inCommon[4] = kCommon;
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT) == std::vector<Bytes>{inCommon});
    h.play.setChatLanguage(kOrcish);
    h.sent.clear();
    REQUIRE(h.play.sendChat(ChatType::Say, "lok'tar"));
    Bytes inOrcish = said("lok'tar");
    inOrcish[4] = kOrcish;
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT) == std::vector<Bytes>{inOrcish});
    // One it does not speak: its own instead (the server would refuse it: LANG_NOT_LEARNED_LANGUAGE).
    h.play.setChatLanguage(14);
    h.sent.clear();
    REQUIRE(h.play.sendChat(ChatType::Say, "hello"));
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT) == std::vector<Bytes>{inCommon});

    // A game master (.gm on) understands everything; the header stays.
    h.apply(values(kPlayer, {{uf::PLAYER_FLAGS, game::kPlayerFlagGameMaster}}));
    const game::ChatLine& gm = h.heard(playerLine(ChatType::Yell, kOrcish, kOrc, "Lok'tar ogar!"));
    CHECK(gm.text == "Lok'tar ogar!");
    CHECK(gm.languageName == "Orcish");

    // As the chat frame writes it (ChatFrame_OnEvent: CHAT_SAY_GET .. "[Orcish] " .. text).
    if (test::loadTexts()) {
        std::string line;
        game::appendChatLine(line, orc, "Bob", 0);
        CHECK(line == "|Hplayer:Bob|h[Bob]|h says: [Orcish] Kazum kaz kaz kaz ");
        line.clear();
        game::appendChatLine(line, yell, "Orgrimmar Grunt", 0);
        CHECK(line == "Orgrimmar Grunt yells: [Orcish] Tar kaz Kazum ");
        text::unload();
    }
}

TEST_CASE("Extras: the small packets vMaNGOS sends, byte for byte, and what 1.12 does with them") {
    // SMSG_MOUNTSPECIAL_ANIM: u64.
    Bytes mount;
    net::ByteWriter(mount).u64v(kOrc);
    u64 unit = 0;
    REQUIRE(net::parseMountSpecialAnim(mount, unit));
    CHECK(unit == kOrc);
    checkTruncations(mount, [&](std::span<const u8> p) { return net::parseMountSpecialAnim(p, unit); });
    // SMSG_PET_ACTION_SOUND: u64, u32.
    Bytes petSound;
    net::ByteWriter(petSound).u64v(kGrunt).u32v(net::kPetTalkAttack);
    net::PetActionSound pet;
    REQUIRE(net::parsePetActionSound(petSound, pet));
    CHECK(pet.pet == kGrunt);
    CHECK(pet.talk == net::kPetTalkAttack);
    checkTruncations(petSound, [&](std::span<const u8> p) { return net::parsePetActionSound(p, pet); });
    // SMSG_INVALIDATE_PLAYER: u64. SMSG_SET_REST_START: u32.
    Bytes invalidate;
    net::ByteWriter(invalidate).u64v(kOrc);
    u64 player = 0;
    REQUIRE(net::parseInvalidatePlayer(invalidate, player));
    CHECK(player == kOrc);
    checkTruncations(invalidate,
                     [&](std::span<const u8> p) { return net::parseInvalidatePlayer(p, player); });
    const Bytes rest{0, 0, 0, 0};
    u32 restTime = 7;
    REQUIRE(net::parseSetRestStart(rest, restTime));
    CHECK(restTime == 0);
    checkTruncations(rest, [&](std::span<const u8> p) { return net::parseSetRestStart(p, restTime); });
    // CMSG_ITEM_NAME_QUERY: u32 entry, u64 0. SMSG_ITEM_NAME_QUERY_RESPONSE: u32 entry, the name and its NUL.
    Bytes query;
    net::writeItemNameQuery(query, 10399);
    CHECK(query == Bytes{0x9F, 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    Bytes nameAnswer;
    net::ByteWriter(nameAnswer).u32v(10399).cstring("Blackened Defias Armor");
    net::ItemNameQueryResponse item;
    REQUIRE(net::parseItemNameQueryResponse(nameAnswer, item));
    CHECK(item.entry == 10399);
    CHECK(item.name == "Blackened Defias Armor");
    checkTruncations(nameAnswer,
                     [&](std::span<const u8> p) { return net::parseItemNameQueryResponse(p, item); });

    Harness h;
    h.play.queries().setOnline(true);
    // Another player's mount rears (0x603FF0); the player's own packet is left alone (it reared on the key).
    REQUIRE(h.packet(Opcode::SMSG_MOUNTSPECIAL_ANIM, mount) == game::Gameplay::Handled::Yes);
    Bytes mine;
    net::ByteWriter(mine).u64v(kPlayer);
    REQUIRE(h.packet(Opcode::SMSG_MOUNTSPECIAL_ANIM, mine) == game::Gameplay::Handled::Yes);
    std::vector<game::FxEvent> rears = h.fx(FxEventKind::MountSpecial);
    REQUIRE(rears.size() == 1);
    CHECK(rears[0].guid == kOrc);
    CHECK(h.packet(Opcode::SMSG_MOUNTSPECIAL_ANIM, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
    // The jump key on a mount standing still: only mounted (CMSG_MOUNTSPECIAL_ANIM has no body).
    CHECK_FALSE(h.play.mountSpecial());
    CHECK(h.sentWith(Opcode::CMSG_MOUNTSPECIAL_ANIM).empty());
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_MOUNTDISPLAYID, 2402}}));
    CHECK(h.play.mountSpecial());
    CHECK(h.sentWith(Opcode::CMSG_MOUNTSPECIAL_ANIM) == std::vector<Bytes>{Bytes{}});
    rears = h.fx(FxEventKind::MountSpecial);
    REQUIRE(rears.size() == 1);
    CHECK(rears[0].guid == kPlayer);

    // The pet's sounds (0x6040C0): order and attack; other values play nothing.
    REQUIRE(h.packet(Opcode::SMSG_PET_ACTION_SOUND, petSound) == game::Gameplay::Handled::Yes);
    Bytes odd;
    net::ByteWriter(odd).u64v(kGrunt).u32v(5);
    REQUIRE(h.packet(Opcode::SMSG_PET_ACTION_SOUND, odd) == game::Gameplay::Handled::Yes);
    const std::vector<game::FxEvent> talks = h.fx(FxEventKind::PetTalk);
    REQUIRE(talks.size() == 1);
    CHECK(talks[0].guid == kGrunt);
    CHECK(talks[0].id == net::kPetTalkAttack);

    // A rename (SMSG_INVALIDATE_PLAYER): a name known is asked for again; one never seen is not.
    Bytes answer;
    net::ByteWriter(answer).u64v(kOrc).cstring("Gruk").cstring("").u32v(2).u32v(0).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_NAME_QUERY_RESPONSE, answer) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_INVALIDATE_PLAYER, invalidate) == game::Gameplay::Handled::Yes);
    Bytes nameQuery;
    net::ByteWriter(nameQuery).u64v(kOrc);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY) == std::vector<Bytes>{nameQuery});
    const std::string* name = h.play.queries().findPlayerName(kOrc);
    REQUIRE(name != nullptr);
    CHECK(*name == "Gruk"); // until the answer
    Bytes renamed;
    net::ByteWriter(renamed).u64v(kOrc).cstring("Grukk").cstring("").u32v(2).u32v(0).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_NAME_QUERY_RESPONSE, renamed) == game::Gameplay::Handled::Yes);
    CHECK(*name == "Grukk"); // the same entry, in place
    Bytes stranger;
    net::ByteWriter(stranger).u64v(0x99);
    REQUIRE(h.packet(Opcode::SMSG_INVALIDATE_PLAYER, stranger) == game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 1);

    // An item set's piece by name only (0x52DF55): asked once; the answer fills it.
    CHECK(h.play.queries().itemName(10399) == nullptr);
    CHECK(h.play.queries().itemName(10399) == nullptr);
    CHECK(h.sentWith(Opcode::CMSG_ITEM_NAME_QUERY) == std::vector<Bytes>{query});
    CHECK(h.sentWith(Opcode::CMSG_ITEM_QUERY_SINGLE).empty());
    REQUIRE(h.packet(Opcode::SMSG_ITEM_NAME_QUERY_RESPONSE, nameAnswer) == game::Gameplay::Handled::Yes);
    const std::string* piece = h.play.queries().itemName(10399);
    REQUIRE(piece != nullptr);
    CHECK(*piece == "Blackened Defias Armor");

    // The rest start: read, nothing done with it (0x5E470A).
    CHECK(h.packet(Opcode::SMSG_SET_REST_START, rest) == game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_SET_REST_START, Bytes{0}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Languages: the cost of a garbled line (desktop)") {
    // A whole chat line (255 bytes) through 0x49B560 with the real words when the packs are there, and a line
    // heard through Gameplay (parse, skill, garbling, the ring).
    const SampleLanguages sample;
    const LanguageData* real = realLanguages();
    const LanguageData& data = real ? *real : sample.data;
    const std::string line =
        "Lok'tar ogar! Victory or death, for the Horde! Ready the wolves and sharpen your axes, "
        "the Alliance will not hold Tarren Mill much longer. Meet at the inn, bring potions, and "
        "do not forget the bandages this time, Grukk. Throm-Ka, brothers and sisters, throm-ka!";
    std::string out;
    out.reserve(2048);
    constexpr u32 kRuns = 20000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRuns; ++i) {
        game::scrambleLanguage(data, kOrcish, i % 3 == 0 ? 0 : 150, line, out);
    }
    const f64 total =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("scrambleLanguage, " << line.size() << " bytes: " << total * 1000.0 / kRuns << " us per line");
    CHECK(total / kRuns < 0.2);

    Harness h;
    const Bytes packet = playerLine(ChatType::Say, kOrcish, kOrc, line);
    const auto heardStart = std::chrono::steady_clock::now();
    constexpr u32 kLines = 5000;
    for (u32 i = 0; i < kLines; ++i) {
        REQUIRE(h.packet(Opcode::SMSG_MESSAGECHAT, packet) == game::Gameplay::Handled::Yes);
    }
    const f64 heard =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - heardStart).count();
    MESSAGE("SMSG_MESSAGECHAT in Orcish through Gameplay: " << heard * 1000.0 / kLines << " us per line");
    CHECK(heard / kLines < 0.5);
}
