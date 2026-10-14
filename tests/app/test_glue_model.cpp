// Phase 18 (glue): the glue screens' rules (app/GlueModel) against GlueXML and WoW.exe 1.12.1: the version
// line (GetBuildInfo 0x46CD70 + VERSION_TEMPLATE), the status dialog of each step (CGlueMgr 0x46B1C9,
// 0x46B880, 0x46B8D0, 0x46BC52), the realm list's load (0x46EC60), type (Cfg_Configs.dbc), colors
// (RealmList.lua) and sort (0x46E9B0), the character selected when the list arrives (0x4724D0), the delete
// confirmation, Config.wtf and realmlist.wtf, and the realm server's address (its order, the dialog).
#include "app/GlueModel.h"
#include "net/ByteBuffer.h"

#include "TextTable.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using namespace vd;

constexpr u32 rgb(u8 r, u8 g, u8 b) {
    return u32{r} | (u32{g} << 8) | (u32{b} << 16) | 0xFF000000u;
}

net::RealmInfo realm(const char* name, u32 icon, u8 flags, f32 population, u8 characters = 0) {
    net::RealmInfo r;
    r.name = name;
    r.icon = icon;
    r.flags = flags;
    r.population = population;
    r.characters = characters;
    return r;
}

} // namespace

TEST_CASE("Glue: the version line of AccountLogin") {
    const std::vector<u8> table = test::textTable({{"VERSION_TEMPLATE", "%s %s (%s) (%s)\n%s"},
                                                   {"BETA_BUILD", "Version"},
                                                   {"RELEASE_BUILD", "Release"}});
    text::setTable(table);
    char out[128];
    CHECK(glueVersionText(out) == "Version 1.12.1 (5875) (Release)\nSep 19 2006");
    text::unload();
}

TEST_CASE("Glue: fades") {
    CHECK(glueFade(0.0f, kLoginFadeIn, true) == doctest::Approx(0.0f));
    CHECK(glueFade(0.75f, kLoginFadeIn, true) == doctest::Approx(0.5f));
    CHECK(glueFade(9.0f, kLoginFadeIn, true) == doctest::Approx(1.0f));
    CHECK(glueFade(0.25f, kLoginFadeOut, false) == doctest::Approx(0.5f));
    CHECK(glueFade(1.0f, 0.0f, false) == doctest::Approx(0.0f));
}

TEST_CASE("Glue: the status dialog of each step") {
    // An empty table: each text is its key's name.
    const std::vector<u8> table = test::textTable({});
    text::setTable(table);
    using Step = GlueSessionState::Step;
    char scratch[256];
    GlueSessionState s;
    CHECK(glueStatus(s, scratch).type == GlueDialogType::None);
    s.step = Step::Connecting;
    GlueStatus st = glueStatus(s, scratch);
    CHECK(st.type == GlueDialogType::Cancel);
    CHECK(st.text == "LOGIN_STATE_CONNECTING");
    CHECK(st.action == GlueStatus::Action::Logout);
    s.step = Step::Authenticating;
    CHECK(glueStatus(s, scratch).text == "LOGIN_STATE_AUTHENTICATING");
    s.step = Step::Authenticated;
    CHECK(glueStatus(s, scratch).text == "LOGIN_STATE_AUTHENTICATED");
    s.step = Step::ChoosingRealm;
    CHECK(glueStatus(s, scratch).type == GlueDialogType::None);
    s.step = Step::WorldConnecting;
    CHECK(glueStatus(s, scratch).text == "GAME_SERVER_LOGIN");
    s.step = Step::CharacterList;
    CHECK(glueStatus(s, scratch).type == GlueDialogType::None);
    s.characterListPending = true;
    CHECK(glueStatus(s, scratch).text == "CHAR_LIST_RETRIEVING");
    s.deleting = true;
    st = glueStatus(s, scratch);
    CHECK(st.text == "CHAR_DELETE_IN_PROGRESS");
    CHECK(st.action == GlueStatus::Action::Close);
    s.realmListPending = true;
    st = glueStatus(s, scratch);
    CHECK(st.text == "REALM_LIST_IN_PROGRESS");
    CHECK(st.action == GlueStatus::Action::CancelRealmList);
    // A failure: OKAY over everything (not while reconnecting).
    s.failure = "Incorrect Password";
    st = glueStatus(s, scratch);
    CHECK(st.type == GlueDialogType::Okay);
    CHECK(st.text == "Incorrect Password");
    s.step = Step::Reconnecting;
    CHECK(glueStatus(s, scratch).text == "VD_LOGIN_RECONNECTING");
    text::unload();
}

TEST_CASE("Glue: the login queue's dialog") {
    const std::vector<u8> table =
        test::textTable({{"QUEUE_NAME_TIME_LEFT_UNKNOWN", "Waiting for %s\nPlace: %d\nTime left: unknown"},
                         {"QUEUE_TIME_LEFT_UNKNOWN", "Waiting for the realm\nPlace: %d\nTime left: unknown"},
                         {"CHANGE_REALM", "Change Realm"}});
    text::setTable(table);
    char scratch[256];
    GlueSessionState s;
    s.step = GlueSessionState::Step::Queued;
    s.queuePosition = 7;
    s.realm = "Dev Realm";
    const GlueStatus st = glueStatus(s, scratch);
    CHECK(st.text == "Waiting for Dev Realm\nPlace: 7\nTime left: unknown");
    CHECK(st.button == "Change Realm");
    CHECK(st.action == GlueStatus::Action::ChangeRealm);
    // The realm list asked for from the queue: no dialog over it.
    s.realmListShown = true;
    CHECK(glueStatus(s, scratch).type == GlueDialogType::None);
    s.realmListShown = false;
    s.realm = {};
    CHECK(glueStatus(s, scratch).text == "Waiting for the realm\nPlace: 7\nTime left: unknown");
    text::unload();
}

TEST_CASE("Glue: realm types (Cfg_Configs.dbc)") {
    bool pvp = false, rp = false;
    realmType(0, pvp, rp);
    CHECK((!pvp && !rp));
    realmType(1, pvp, rp);
    CHECK((pvp && !rp));
    realmType(6, pvp, rp);
    CHECK((!pvp && rp));
    realmType(8, pvp, rp);
    CHECK((pvp && rp));
    realmType(200, pvp, rp);
    CHECK((!pvp && !rp));
}

TEST_CASE("Glue: realm rows, loads and colors as RealmListUpdate") {
    const std::vector<u8> table = test::textTable({});
    text::setTable(table);
    const std::vector<net::RealmInfo> realms = {
        realm("Kirin Tor", 6, 0, 1.0f, 2),                    // RP, its population the mean
        realm("Archimonde", 1, 0, 0.1f),                      // PvP, quiet
        realm("Blackrock", 1, 0, 1.9f),                       // PvP, crowded
        realm("Dalaran", 0, net::kRealmFlagNewPlayers, 1.0f), // 0x20: Recommended
        realm("Elune", 0, net::kRealmFlagRecommended, 1.0f),  // 0x40: New
        realm("Frostmane", 8, net::kRealmFlagFull, 1.0f),     // RPPVP, full
        realm("Gurubashi", 1, net::kRealmFlagOffline | net::kRealmFlagInvalid, 0.0f),
    };
    std::vector<RealmRow> rows;
    RealmSort sort;
    realmRows(realms, "Elune", sort, rows);
    REQUIRE(rows.size() == 7);
    // By name, as SortRealms("name") first.
    CHECK(rows[0].name == "Archimonde");
    CHECK(rows[6].name == "Kirin Tor");
    auto find = [&](std::string_view name) -> const RealmRow& {
        for (const RealmRow& r : rows) {
            if (r.name == name) {
                return r;
            }
        }
        return rows[0];
    };
    CHECK(find("Kirin Tor").load == RealmLoad::Medium);
    CHECK(find("Archimonde").load == RealmLoad::Low);
    CHECK(find("Blackrock").load == RealmLoad::High);
    CHECK(find("Dalaran").load == RealmLoad::Recommended);
    CHECK(find("Elune").load == RealmLoad::New);
    CHECK(find("Frostmane").load == RealmLoad::Full);
    CHECK(realmIsFull(find("Frostmane")));
    CHECK(find("Elune").current);
    CHECK_FALSE(find("Dalaran").current);
    const RealmRow& down = find("Gurubashi");
    CHECK(down.down);
    CHECK(down.invalid);
    CHECK(realmLoadText(down) == "REALM_DOWN");
    CHECK(realmLoadColor(down) == rgb(128, 128, 128));
    CHECK(realmNameColor(down) == rgb(128, 128, 128));
    // Names: green with characters, gold without, red when invalid.
    CHECK(realmNameColor(find("Kirin Tor")) == rgb(26, 255, 26));
    CHECK(realmNameColor(find("Dalaran")) == rgb(255, 199, 0));
    // Columns' texts and colors.
    CHECK(realmTypeText(find("Kirin Tor")) == "RP_PARENTHESES");
    CHECK(realmTypeColor(find("Kirin Tor")) == rgb(26, 255, 26));
    CHECK(realmTypeText(find("Archimonde")) == "PVP_PARENTHESES");
    CHECK(realmTypeColor(find("Archimonde")) == rgb(255, 26, 26));
    CHECK(realmTypeText(find("Frostmane")) == "RPPVP_PARENTHESES");
    CHECK(realmTypeText(find("Dalaran")) == "GAMETYPE_NORMAL");
    CHECK(realmLoadText(find("Dalaran")) == "LOAD_RECOMMENDED");
    CHECK(realmLoadColor(find("Dalaran")) == rgb(0, 191, 243));
    CHECK(realmLoadText(find("Blackrock")) == "LOAD_HIGH");
    CHECK(realmLoadColor(find("Archimonde")) == rgb(26, 255, 26));

    // One realm: its population against 1 +- 0.
    std::vector<RealmRow> one;
    realmRows(std::span<const net::RealmInfo>(realms.data(), 1), "", sort, one);
    CHECK(one[0].load == RealmLoad::Medium);
    realmRows(std::span<const net::RealmInfo>(realms.data() + 1, 1), "", sort, one);
    CHECK(one[0].load == RealmLoad::Low);
    text::unload();
}

TEST_CASE("Glue: SortRealms") {
    const std::vector<net::RealmInfo> realms = {realm("B", 1, 0, 1.0f, 2), realm("A", 0, 0, 1.0f, 5),
                                                realm("C", 6, 0, 1.0f, 2)};
    std::vector<RealmRow> rows;
    RealmSort sort;
    realmRows(realms, "", sort, rows);
    CHECK(rows[0].name == "A");
    CHECK(rows[2].name == "C");
    // The same key again: the other way round.
    sort.by(RealmSortKey::Name);
    realmRows(realms, "", sort, rows);
    CHECK(rows[0].name == "C");
    CHECK(rows[2].name == "A");
    // Characters first, then the name (now descending) for the ties.
    sort.by(RealmSortKey::Characters);
    CHECK(sort.keys[0].key == RealmSortKey::Characters);
    CHECK(sort.keys[1].key == RealmSortKey::Name);
    realmRows(realms, "", sort, rows);
    CHECK(rows[0].name == "C");
    CHECK(rows[1].name == "B");
    CHECK(rows[2].name == "A");
    // Mode: Normal, PVP, RP.
    sort.by(RealmSortKey::Mode);
    realmRows(realms, "", sort, rows);
    CHECK(rows[0].name == "A");
    CHECK(rows[1].name == "B");
    CHECK(rows[2].name == "C");
}

TEST_CASE("Glue: the character the list selects") {
    CHECK(selectedCharacter(0, 3, CharacterListReason::Login) == -1);
    CHECK(selectedCharacter(5, 3, CharacterListReason::Login) == 3);
    CHECK(selectedCharacter(3, 3, CharacterListReason::Login) == 0); // beyond the list
    CHECK(selectedCharacter(3, -1, CharacterListReason::Login) == 0);
    CHECK(selectedCharacter(4, 1, CharacterListReason::Created) == 3);
    CHECK(selectedCharacter(4, 2, CharacterListReason::Deleted) == 0);
}

TEST_CASE("Glue: the delete confirmation") {
    const std::vector<u8> table = test::textTable({{"DELETE_CONFIRM_STRING", "DELETE"}});
    text::setTable(table);
    CHECK(deleteConfirmed("DELETE"));
    CHECK(deleteConfirmed("delete"));
    CHECK(deleteConfirmed("Delete"));
    CHECK_FALSE(deleteConfirmed("DELET"));
    CHECK_FALSE(deleteConfirmed("DELETE "));
    CHECK_FALSE(deleteConfirmed(""));
    text::unload();
}

TEST_CASE("Glue: Config.wtf") {
    GlueConfig config;
    config.accountName = "ADMIN";
    config.realmName = "Dev Realm";
    config.realmList = "192.168.1.20:3724";
    config.lastCharacterIndex = 2;
    const std::string written = writeGlueConfig(config);
    CHECK(written == "SET accountName \"ADMIN\"\nSET realmName \"Dev Realm\"\n"
                     "SET realmList \"192.168.1.20:3724\"\nSET lastCharacterIndex \"2\"\n");
    const GlueConfig read = parseGlueConfig(written);
    CHECK(read.loaded);
    CHECK(read.accountName == "ADMIN");
    CHECK(read.realmName == "Dev Realm");
    CHECK(read.realmList == "192.168.1.20:3724");
    CHECK(read.lastCharacterIndex == 2);
    // The client's own lines (other CVars, Windows line ends, "set" in lower case) are read too.
    const GlueConfig wtf = parseGlueConfig("SET gxResolution \"1024x768\"\r\nset accountName \"PLAYER1\"\r\n"
                                           "SET realmName \"Other Realm\"\r\nbroken line\r\n");
    CHECK(wtf.accountName == "PLAYER1");
    CHECK(wtf.realmName == "Other Realm");
    CHECK(wtf.realmList.empty());
    CHECK(wtf.lastCharacterIndex == 0);
}

TEST_CASE("Glue: the realmList CVar in any case, quoted or not (Config.wtf, realmlist.wtf)") {
    CHECK(parseGlueConfig("SET realmList \"logon.example.org\"\n").realmList == "logon.example.org");
    CHECK(parseGlueConfig("set realmlist logon.example.org").realmList == "logon.example.org");
    CHECK(parseGlueConfig("SET REALMLIST \"10.0.0.5:3725\"\r\n").realmList == "10.0.0.5:3725");
    CHECK(parseGlueConfig("  set\trealmlist   logon.example.org  \r\n").realmList == "logon.example.org");
    CHECK(parseGlueConfig("SeT ReAlMlIsT a.example.org\nSET realmList b.example.org\n").realmList ==
          "b.example.org");
    // realmlist.wtf as Notepad may save it (a UTF-8 byte order mark), with 1.12's patch list too.
    const GlueConfig file =
        parseGlueConfig("\xEF\xBB\xBFset realmlist logon.example.org\r\nset patchlist patch.example.org\r\n");
    CHECK(file.realmList == "logon.example.org");
    CHECK(file.accountName.empty());
    // Not the realmList: nothing.
    CHECK(parseGlueConfig("set realmlistx a.example.org\nrealmlist a.example.org\n").realmList.empty());
    CHECK(parseGlueConfig("SETrealmList x\n").realmList.empty());
    CHECK(parseGlueConfig("").realmList.empty());
}

TEST_CASE("Glue: the realm server's order (launch, the player's own, client.cfg)") {
    CHECK(glueRealmList("10.0.0.1:3724", "logon.example.org", "127.0.0.1") == "10.0.0.1:3724");
    CHECK(glueRealmList("", "logon.example.org", "127.0.0.1") == "logon.example.org");
    CHECK(glueRealmList("", "", "127.0.0.1:3724") == "127.0.0.1:3724");
    CHECK(glueRealmList(" ", "\t", "  10.0.0.2  ") == "10.0.0.2");
    // A fresh install: none, and LOGIN asks for one.
    CHECK(glueRealmList("", "", "").empty());
    CHECK(glueNeedsServer(glueRealmList("", "", "")));
    CHECK(glueNeedsServer("  "));
    CHECK_FALSE(glueNeedsServer("logon.example.org"));
}

TEST_CASE("Glue: the address dialog's OKAY") {
    // A host name, host:port, an address: kept as typed (the spaces around it dropped).
    GlueServerEntry e = glueServerEntry("logon.example.org", "", false);
    CHECK(e.close);
    CHECK(e.realmList == "logon.example.org");
    CHECK(e.server == "logon.example.org");
    CHECK(e.error.empty());
    e = glueServerEntry("  192.168.1.20:3725 ", "", true);
    CHECK(e.close);
    CHECK(e.realmList == "192.168.1.20:3725");
    CHECK(e.server == "192.168.1.20:3725");
    // A line of realmlist.wtf pasted whole.
    e = glueServerEntry("set realmlist logon.example.org", "", false);
    CHECK(e.close);
    CHECK(e.realmList == "logon.example.org");
    e = glueServerEntry("SET realmList \"logon.example.org:3724\"", "", false);
    CHECK(e.realmList == "logon.example.org:3724");
    // A bad port, or no host: what is wrong, and the dialog stays.
    for (const char* bad : {"logon.example.org:", "host:0", "host:65536", "host:37a4", "host:-1"}) {
        INFO(bad);
        e = glueServerEntry(bad, "", false);
        CHECK_FALSE(e.close);
        CHECK(e.error == "VD_LOGIN_BAD_PORT");
    }
    for (const char* noHost : {":3724", ":", "set realmlist :3724"}) {
        INFO(noHost);
        e = glueServerEntry(noHost, "127.0.0.1", false);
        CHECK_FALSE(e.close);
        CHECK(e.error == "VD_LOGIN_ENTER_SERVER");
    }
    // Empty: the player's own forgotten, back to client.cfg's (or the launch's)...
    e = glueServerEntry("  ", "127.0.0.1:3724", false);
    CHECK(e.close);
    CHECK(e.realmList.empty());
    CHECK(e.server == "127.0.0.1:3724");
    e = glueServerEntry("", "", false);
    CHECK(e.close);
    CHECK(e.server.empty());
    // ...but LOGIN cannot go on without any.
    e = glueServerEntry("", "", true);
    CHECK_FALSE(e.close);
    CHECK(e.error == "VD_LOGIN_ENTER_SERVER");
    e = glueServerEntry("", "127.0.0.1", true);
    CHECK(e.close);
    CHECK(e.server == "127.0.0.1");
}
