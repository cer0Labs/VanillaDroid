// Phase 13 (social): the rest of ChatFrame.lua's slash commands (their English names from GlobalStrings.lua's
// SLASH_*, then the Spanish aliases), the line a player types as the client sends it (typed bars escaped, inserted
// links kept: what vMaNGOS' ChatHandler::isValidChatMessage accepts) and the links the windows insert
// (GetContainerItemLink's format, WoW.exe 1.12.1 "%s|Hitem:%d:%d:%d:%d|h[%s]|h%s"), through app/ChatLinkInsert
// into the chat's text field.
#include "app/ChatLinkInsert.h"
#include "game/ChatCommands.h"
#include "game/ChatLinks.h"
#include "ui/Context.h"
#include "ui/TextInput.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>

namespace {

using namespace vd;
using game::ChatCommandKind;
using K = ChatCommandKind;

game::ChatCommand parse(std::string_view input) {
    return game::parseChatInput(input, net::ChatType::Say);
}

// vMaNGOS' ChatStrictLinkChecking.Severity 2 (the server's setting): every "|" starts one of c H h r or is escaped
// ("||"), and c H h h r come in that order.
bool serverAccepts(std::string_view msg) {
    const std::string_view valid = "cHhr|";
    const char sequence[] = "cHhhr";
    u32 at = 0;
    for (usize i = 0; i < msg.size(); ++i) {
        if (msg[i] != '|') {
            continue;
        }
        if (i + 1 >= msg.size()) {
            return false;
        }
        const char command = msg[++i];
        if (valid.find(command) == std::string_view::npos) {
            return false;
        }
        if (command == sequence[at]) {
            at = at == 4 ? 0 : at + 1;
        } else if (command != '|') {
            return false;
        }
    }
    return msg.size() <= 255;
}

} // namespace

TEST_CASE("Slash commands: phase 13's, with their aliases") {
    for (const char* line : {"/follow", "/f", "/fol", "/seguir"}) {
        CHECK(parse(line).kind == K::Follow);
        CHECK(parse(line).target.empty()); // FollowUnit("target")
    }
    game::ChatCommand c = parse("/follow Aeryn");
    CHECK(c.kind == K::Follow);
    CHECK(c.target == "Aeryn");
    c = parse("/target Stormwind City Guard"); // GetSlashCmdTarget keeps the whole rest
    CHECK(c.kind == K::TargetByName);
    CHECK(c.target == "Stormwind City Guard");
    CHECK(parse("/tar hog").kind == K::TargetByName);
    CHECK(parse("/objetivo hog").kind == K::TargetByName);
    CHECK(parse("/assist").kind == K::Assist);
    CHECK(parse("/a Aeryn").target == "Aeryn");
    CHECK(parse("/inspect").kind == K::Inspect);
    CHECK(parse("/ins").kind == K::Inspect);
    CHECK(parse("/trade").kind == K::Trade);
    CHECK(parse("/tr").kind == K::Trade);
    CHECK(parse("/pvp").kind == K::Pvp);
    CHECK(parse("/played").kind == K::Played);
    CHECK(parse("/jugado").kind == K::Played);
    CHECK(parse("/time").kind == K::Time);
    c = parse("/cast Fireball(Rank 3)");
    CHECK(c.kind == K::Cast);
    CHECK(c.text == "Fireball(Rank 3)");
    CHECK(parse("/spell Frostbolt").text == "Frostbolt");
    CHECK(parse("/gleader Aeryn").kind == K::GuildLeader);
    CHECK(parse("/gleader Aeryn").target == "Aeryn");
    CHECK(parse("/guilddisband").kind == K::GuildDisband);
    CHECK(parse("/gdisband").kind == K::GuildDisband);
    CHECK(parse("/readycheck").kind == K::ReadyCheck);
    CHECK(parse("/raidinfo").kind == K::RaidInfo);
    CHECK(parse("/ffa").kind == K::LootFreeForAll);
    CHECK(parse("/roundrobin").kind == K::LootRoundRobin);
    CHECK(parse("/master Aeryn").kind == K::LootMaster);
    CHECK(parse("/chathelp").kind == K::ChatHelp);
    CHECK(parse("/script print(1)").kind == K::Script);
    CHECK(parse("/run x").kind == K::Script);
    // Aliases of phase 8's commands from SLASH_*.
    CHECK(parse("/i Aeryn").kind == K::Invite);
    CHECK(parse("/u Aeryn").kind == K::Uninvite);
    CHECK(parse("/un Aeryn").kind == K::Uninvite);
    CHECK(parse("/pr Aeryn").kind == K::Promote);
    CHECK(parse("/remfriend Aeryn").kind == K::RemoveFriend);
    CHECK(parse("/chan secretos").kind == K::Join);
    CHECK(parse("/chatexit 4").kind == K::Leave);
    CHECK(parse("/chatinfo").kind == K::ChannelList);
    CHECK(parse("/rand 10").kind == K::Roll);
    CHECK(parse("/rw pull").type == net::ChatType::RaidWarning);
    CHECK(parse("/rsay hi").type == net::ChatType::Raid);
    CHECK(parse("/osay hi").type == net::ChatType::Officer);
    CHECK(parse("/gc hi").type == net::ChatType::Guild);
    CHECK(parse("/bg inc").type == net::ChatType::Battleground);
    CHECK(parse("/ginfo").kind == K::GuildInfo);
    CHECK(parse("/guildinfo").kind == K::GuildInfo);
    CHECK(parse("/groster").kind == K::GuildRoster);
    CHECK(parse("/guildinvite Aeryn").kind == K::GuildInvite);
    CHECK(parse("/guildquit").kind == K::GuildLeave);
    // SLASH_CHANNEL: /c N text, as /N text.
    game::ChatCommand ch = parse("/c 2 WTS cloth");
    CHECK(ch.kind == K::Message);
    CHECK(ch.type == net::ChatType::Channel);
    CHECK(ch.channel == 2);
    CHECK(ch.text == "WTS cloth");
    CHECK(parse("/csay 4").kind == K::SetChannel);
    // "/kick" is the group's uninvite; the channel's is "/ckick".
    CHECK(parse("/kick Aeryn").kind == K::Uninvite);
}

TEST_CASE("Slash commands: the channels' moderation") {
    struct Case {
        const char* line;
        K kind;
    };
    const Case cases[] = {{"/password secretos abc", K::ChannelPassword}, {"/pass secretos abc", K::ChannelPassword},
                          {"/owner secretos Galen", K::ChannelOwner},     {"/mod secretos Galen", K::ChannelModerator},
                          {"/moderator secretos Galen", K::ChannelModerator},
                          {"/unmod secretos Galen", K::ChannelUnmoderator},
                          {"/mute secretos Galen", K::ChannelMute},       {"/squelch secretos Galen", K::ChannelMute},
                          {"/unvoice secretos Galen", K::ChannelMute},    {"/unmute secretos Galen", K::ChannelUnmute},
                          {"/voice secretos Galen", K::ChannelUnmute},    {"/cinvite secretos Galen", K::ChannelInvite},
                          {"/chatinvite secretos Galen", K::ChannelInvite}, {"/ckick secretos Galen", K::ChannelKick},
                          {"/ban secretos Galen", K::ChannelBan},         {"/unban secretos Galen", K::ChannelUnban},
                          {"/silenciar secretos Galen", K::ChannelMute},  {"/vetar secretos Galen", K::ChannelBan}};
    for (const Case& t : cases) {
        INFO(t.line);
        const game::ChatCommand c = parse(t.line);
        CHECK(c.kind == t.kind);
        CHECK(c.target == "secretos");
        CHECK((c.text == "Galen" || c.text == "abc"));
    }
    game::ChatCommand c = parse("/owner 5");
    CHECK(c.kind == K::ChannelOwner);
    CHECK(c.channel == 5); // a channel's number: its name is looked up
    CHECK(c.text.empty()); // DisplayChannelOwner
    c = parse("/password secretos");
    CHECK(c.text.empty()); // the password taken off
    CHECK(parse("/announce secretos").kind == K::ChannelAnnounce);
    CHECK(parse("/ann 4").channel == 4);
    CHECK(parse("/moderate secretos").kind == K::ChannelModerate);
}

TEST_CASE("Chat links: the line the client sends") {
    std::string out;
    char link[160];
    const std::string_view sword = game::formatItemLink(link, 2132, "|cff1eff00", "Militia Shortsword");
    // A link inserted whole goes as it is; what was typed around it too.
    std::string typed = "WTS ";
    typed.append(sword).append(" 1g");
    game::escapeChatInput(out, typed);
    CHECK(out == typed);
    CHECK(serverAccepts(out));
    // A typed bar is escaped, so a "|c" or a "|H" the player typed shows as typed and passes the server.
    game::escapeChatInput(out, "a|b ||c |cffff0000red|r |Hitem:1|h[x]|h");
    CHECK(out == "a||b ||||c ||cffff0000red||r ||Hitem:1||h[x]||h");
    CHECK(serverAccepts(out));
    CHECK_FALSE(serverAccepts("a|b")); // what the server would drop
    // Too long: cut before what does not fit, never inside a link.
    std::string many;
    for (int i = 0; i < 6; ++i) {
        many.append(sword);
    }
    game::escapeChatInput(out, many);
    CHECK(out.size() <= 255);
    CHECK(out.size() % sword.size() == 0);
    CHECK(serverAccepts(out));

    // An item the player has: its enchantment and random property, its suffix after its name.
    const std::string_view bracers = game::formatItemLink(link, 15210, "|cff1eff00", "Raider Shortsword", 1897, 1127,
                                                          "of the Monkey");
    CHECK(bracers == "|cff1eff00|Hitem:15210:1897:1127:0|h[Raider Shortsword of the Monkey]|h|r");
    game::ChatLink parsed;
    REQUIRE(game::nextChatLink(bracers, 0, parsed));
    CHECK(parsed.kind == game::ChatLinkKind::Item);
    CHECK(parsed.id == 15210);
    CHECK(parsed.values[0] == 1897);
    CHECK(parsed.values[1] == 1127);
    CHECK(serverAccepts(bracers));
}

TEST_CASE("Chat links: ChatLinkInsert puts them in the open chat") {
    ui::TextInput input;
    ChatLinkInsert insert;
    insert.bind(&input);
    char link[160];
    const std::string_view sword = game::formatItemLink(link, 2132, "|cff1eff00", "Militia Shortsword");
    CHECK_FALSE(insert.active()); // the edit box is closed: the tap acts as always
    CHECK_FALSE(insert.insert(sword));
    input.begin(ui::widgetId("social.add"), "x", 47);
    CHECK_FALSE(insert.active()); // another field
    input.begin(ui::widgetId("chat.input"), "WTS ", 255, ui::TextKind::Message);
    REQUIRE(insert.active());
    const u32 version = input.version();
    REQUIRE(insert.insert(sword));
    CHECK(input.text() == std::string("WTS ") + std::string(sword));
    CHECK(input.version() != version); // the IME is re-seeded with it, the caret at its end (main_android.cpp)
    // A link that does not fit is not cut: nothing goes in.
    std::string full(240, 'x');
    input.begin(ui::widgetId("chat.input"), full, 255, ui::TextKind::Message);
    CHECK_FALSE(insert.insert(sword));
    CHECK(input.text() == full);
}
