// The chat's slash commands (phase 8) and the social client tables: zone channels, channel kinds, text emotes and
// race and class names from the real data (db.vdpk of the working tree; skipped without it).
#include "TextTable.h"
#include "core/Text.h"
#include "game/ChatCommands.h"
#include "game/SocialData.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using game::ChatCommandKind;
using net::ChatType;

game::ChatCommand parse(std::string_view input, ChatType current = ChatType::Say, std::string_view target = {},
                        u32 channel = 0) {
    return game::parseChatInput(input, current, target, channel);
}

} // namespace

TEST_CASE("Chat input: plain text, channels, whispers, commands, emotes") {
    game::ChatCommand c = parse("  hola a todos ");
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Say);
    CHECK(c.text == "hola a todos");
    CHECK(parse("").kind == ChatCommandKind::None);
    CHECK(parse("   ").kind == ChatCommandKind::None);
    // The current channel: a whisper keeps its name, a numbered channel its number.
    c = parse("voy", ChatType::Whisper, "Aeryn");
    CHECK(c.type == ChatType::Whisper);
    CHECK(c.target == "Aeryn");
    c = parse("WTS espada", ChatType::Channel, {}, 2);
    CHECK(c.type == ChatType::Channel);
    CHECK(c.channel == 2);
    // A server command goes out as said whatever the channel.
    c = parse(".tele goldshire", ChatType::Party);
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Say);
    CHECK(c.text == ".tele goldshire");

    c = parse("/p los kobolds son mios");
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Party);
    CHECK(c.text == "los kobolds son mios");
    CHECK(parse("/G hola").type == ChatType::Guild); // case insensitive
    CHECK(parse("/y ayuda").type == ChatType::Yell);
    CHECK(parse("/o").kind == ChatCommandKind::SetChannel);
    CHECK(parse("/o").type == ChatType::Officer);
    CHECK(parse("/grupo hola").type == ChatType::Party);
    CHECK(parse("/e baila").type == ChatType::Emote);
    CHECK(parse("/e").kind == ChatCommandKind::None);

    c = parse("/2 WTS [Linen Cloth]");
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Channel);
    CHECK(c.channel == 2);
    CHECK(c.text == "WTS [Linen Cloth]");
    c = parse("/1");
    CHECK(c.kind == ChatCommandKind::SetChannel);
    CHECK(c.channel == 1);
    CHECK(parse("/11 hola").kind == ChatCommandKind::Unknown);

    c = parse("/w Aeryn vienes?");
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Whisper);
    CHECK(c.target == "Aeryn");
    CHECK(c.text == "vienes?");
    c = parse("/tell Aeryn");
    CHECK(c.kind == ChatCommandKind::SetChannel);
    CHECK(c.target == "Aeryn");
    CHECK(parse("/w").kind == ChatCommandKind::Unknown);
    c = parse("/r gracias");
    CHECK(c.kind == ChatCommandKind::Reply);
    CHECK(c.text == "gracias");

    c = parse("/join Prueba clave");
    CHECK(c.kind == ChatCommandKind::Join);
    CHECK(c.target == "Prueba");
    CHECK(c.text == "clave");
    CHECK(parse("/join").kind == ChatCommandKind::Unknown);
    c = parse("/leave 3");
    CHECK(c.kind == ChatCommandKind::Leave);
    CHECK(c.channel == 3);
    c = parse("/leave Prueba");
    CHECK(c.target == "Prueba");
    CHECK(c.channel == 0);
    CHECK(parse("/chatlist 4").channel == 4);

    c = parse("/invite Brakk");
    CHECK(c.kind == ChatCommandKind::Invite);
    CHECK(c.target == "Brakk");
    CHECK(parse("/inv").target.empty()); // the target
    CHECK(parse("/kick Brakk").kind == ChatCommandKind::Uninvite);
    CHECK(parse("/leaveparty").kind == ChatCommandKind::LeaveGroup);
    CHECK(parse("/promote Brakk").kind == ChatCommandKind::Promote);
    c = parse("/afk comiendo algo");
    CHECK(c.kind == ChatCommandKind::Afk);
    CHECK(c.text == "comiendo algo");
    CHECK(parse("/dnd").kind == ChatCommandKind::Dnd);
    CHECK(parse("/who 10-20 elwynn").text == "10-20 elwynn");
    CHECK(parse("/friend Tolvir").kind == ChatCommandKind::AddFriend);
    CHECK(parse("/ignore Tolvir").kind == ChatCommandKind::Ignore);
    CHECK(parse("/unignore Tolvir").kind == ChatCommandKind::Unignore);
    CHECK(parse("/ginvite Brakk").kind == ChatCommandKind::GuildInvite);
    CHECK(parse("/gquit").kind == ChatCommandKind::GuildLeave);
    CHECK(parse("/gmotd Raid el viernes").text == "Raid el viernes");
    CHECK(parse("/duel").kind == ChatCommandKind::Duel);
    CHECK(parse("/forfeit").kind == ChatCommandKind::Forfeit);
    CHECK(parse("/ayuda").kind == ChatCommandKind::Help);

    c = parse("/roll");
    CHECK(c.kind == ChatCommandKind::Roll);
    CHECK(c.minimum == 1);
    CHECK(c.maximum == 100);
    c = parse("/roll 20");
    CHECK(c.maximum == 20);
    c = parse("/roll 10 30");
    CHECK(c.minimum == 10);
    CHECK(c.maximum == 30);

    // Anything else may be a text emote.
    c = parse("/wave");
    CHECK(c.kind == ChatCommandKind::TextEmote);
    CHECK(c.target == "wave");
    CHECK(parse("/").kind == ChatCommandKind::Unknown);
}

TEST_CASE("Chat input: the commands of the client's language (phase 21, esES GlobalStrings)") {
    // As the esES client names them: "/s" both SLASH_SAY1 and SLASH_WHISPER1, "/g" SLASH_PARTY1 and SLASH_GUILD5, "/h"
    // SLASH_GUILD1 and SLASH_HELP1, "/canal" SLASH_JOIN2 (our own Spanish alias made it the channel list).
    const std::vector<vd::u8> table = vd::test::textTable({
        {"SLASH_SAY1", "/s"},          {"SLASH_SAY4", "/decir"},     {"SLASH_WHISPER1", "/s"},
        {"SLASH_WHISPER2", "/susurrar"}, {"SLASH_PARTY1", "/g"},     {"SLASH_PARTY2", "/grupo"},
        {"SLASH_GUILD1", "/h"},        {"SLASH_GUILD5", "/g"},       {"SLASH_HELP1", "/h"},
        {"SLASH_HELP6", "/ayuda"},     {"SLASH_JOIN2", "/canal"},    {"SLASH_WHO1", "/quien"},
        {"SLASH_CHAT_PASSWORD3", "/contraseña"}, {"SLASH_CAST1", "/lanzar"},
    });
    text::setTable(table);
    game::ChatCommand c = parse("/s hola");
    CHECK(c.kind == ChatCommandKind::Message);
    CHECK(c.type == ChatType::Say);
    c = parse("/susurrar Aeryn hola");
    CHECK(c.type == ChatType::Whisper);
    CHECK(c.target == "Aeryn");
    CHECK(parse("/g hola").type == ChatType::Party);
    CHECK(parse("/G hola").type == ChatType::Party);
    CHECK(parse("/h hola").type == ChatType::Guild);
    CHECK(parse("/ayuda").kind == ChatCommandKind::Help);
    c = parse("/canal Pruebas");
    CHECK(c.kind == ChatCommandKind::Join);
    CHECK(c.target == "Pruebas");
    CHECK(parse("/quien c-war").kind == ChatCommandKind::Who);
    CHECK(parse("/contraseña Pruebas 123").kind == ChatCommandKind::ChannelPassword);
    CHECK(parse("/lanzar Fuego").kind == ChatCommandKind::Cast);
    // What the client's language does not name still goes by the tables' names.
    CHECK(parse("/dados").kind == ChatCommandKind::Roll);
    CHECK(parse("/wave").kind == ChatCommandKind::TextEmote);
    text::unload();
}

TEST_CASE("Social data: zones, the channels the client joins in them, text emotes") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::SocialData data;
    REQUIRE(data.init(db));
    CHECK(data.zoneOf(87) == 12); // Goldshire is in Elwynn Forest
    CHECK(data.zoneOf(12) == 12);
    CHECK(data.zoneOf(999999) == 0);
    CHECK(data.areaName(12) == "Elwynn Forest");
    CHECK(data.isCity(1519));
    CHECK_FALSE(data.isCity(12));

    std::span<const game::ZoneChannel> zone = data.zoneChannels(12);
    REQUIRE(zone.size() == 4);
    CHECK(zone[0].id == 1);
    CHECK(zone[0].name == "General - Elwynn Forest");
    CHECK(zone[1].id == 2);
    CHECK(zone[1].name.empty()); // no trade outside the cities
    CHECK(zone[2].id == 22);
    CHECK(zone[2].name == "LocalDefense - Elwynn Forest");
    CHECK(zone[3].id == 24);
    CHECK(zone[3].name == "LookingForGroup");
    zone = data.zoneChannels(1519);
    CHECK(zone[0].name == "General - Stormwind City");
    CHECK(zone[1].name == "Trade - City");
    CHECK(data.channelKind("Trade - City") == 2);
    CHECK(data.channelKind("General - Durotar") == 1);
    CHECK(data.channelKind("Prueba") == 0);

    const game::SocialData::TextEmote* wave = data.textEmoteByToken("Wave");
    REQUIRE(wave);
    CHECK(wave->id == 101);
    CHECK(wave->token == "wave");
    CHECK(data.textEmoteLine(101, game::SocialData::kEmoteOthersTarget) == "%s waves at %s.");
    CHECK(data.textEmoteLine(101, game::SocialData::kEmoteYou) == "You wave.");
    CHECK(data.textEmoteLine(101, game::SocialData::kEmoteToYou) == "%s waves at you.");
    CHECK(data.textEmoteByToken("dance"));
    CHECK_FALSE(data.textEmoteByToken("notanemote"));

    // Race and class names (ChrRaces / ChrClasses, enUS).
    CHECK(data.raceName(1) == "Human");
    CHECK(data.raceName(4) == "Night Elf");
    CHECK(data.raceName(5) == "Undead");
    CHECK(data.raceName(8) == "Troll");
    CHECK(data.raceName(99).empty());
    CHECK(data.className(1) == "Warrior");
    CHECK(data.className(4) == "Rogue");
    CHECK(data.className(11) == "Druid");
    CHECK(data.className(6).empty()); // no class 6 in 1.12
}
