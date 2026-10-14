// Phase 18 (extras): the spell book's new-spell flash (app/SpellBookFlash) as SpellBookFrame.lua runs it
// in 1.12: LEARNED_SPELL_IN_TAB, SpellBookFrame_OnShow / OnHide, SpellBookSkillLineTab_OnClick and
// UIFrameFlash's timing; and a renamed character's WTF folders (app/WtfFolders, WoW.exe 0x4725F0).
#include "app/SpellBookFlash.h"
#include "app/WtfFolders.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace vd;

TEST_CASE("Spell book flash: LEARNED_SPELL_IN_TAB flashes the tab once the book shows, for 30 s") {
    SpellBookFlash flash;
    // Nothing learned: nothing flashes.
    flash.shown(1000);
    CHECK(flash.alpha(0, 1500) == 0.0f);
    flash.hidden();

    // A spell learned in the second tab while the book is closed: its texture waits for the book.
    flash.learned(1, false);
    CHECK(flash.pending(1));
    CHECK(flash.alpha(1, 2000) == 0.0f);
    flash.shown(10000); // SpellBookFrame_OnShow: UIFrameFlash(frame, 0.5, 0.5, 30)
    CHECK(flash.alpha(1, 10000) == doctest::Approx(0.0f));
    CHECK(flash.alpha(1, 10250) == doctest::Approx(0.5f)); // fading in
    CHECK(flash.alpha(1, 10500) == doctest::Approx(1.0f));
    CHECK(flash.alpha(1, 10750) == doctest::Approx(0.5f)); // fading out
    CHECK(flash.alpha(1, 11250) == doctest::Approx(0.5f)); // and again
    CHECK(flash.alpha(0, 10500) == 0.0f);                  // only its tab
    CHECK(flash.alpha(1, 10000 + 30001) == 0.0f);          // over after 30 s

    // A click on the tab hides its texture; the others keep flashing.
    flash.hidden();
    flash.learned(0, false);
    flash.learned(2, false);
    flash.shown(50000);
    flash.clicked(0);
    CHECK(flash.alpha(0, 50500) == 0.0f);
    CHECK(flash.alpha(2, 50500) == doctest::Approx(1.0f));
    // Closing the book stops the flash and hides every texture: reopened, nothing flashes.
    flash.hidden();
    CHECK_FALSE(flash.pending(2));
    flash.shown(60000);
    CHECK(flash.alpha(2, 60500) == 0.0f);

    // Learned while the book is open and flashing: it flashes with the frame; once the 30 s are over, not
    // until the book shows again (and closing it hides the texture).
    flash.hidden();
    flash.learned(1, false);
    flash.shown(70000);
    flash.learned(3, false);
    CHECK(flash.alpha(3, 70500) == doctest::Approx(1.0f));
    flash.learned(4, false);
    CHECK(flash.alpha(4, 70000 + 31000) == 0.0f);

    // The pet's page (BOOKTYPE_PET) as the book's last: LEARNED_SPELL_IN_TAB is ignored; past the eighth tab
    // too.
    flash.hidden();
    flash.learned(1, true);
    flash.learned(SpellBookFlash::kMaxTabs, false);
    flash.shown(80000);
    CHECK(flash.alpha(1, 80500) == 0.0f);
}

TEST_CASE("WTF folders: a renamed character takes its folders along (0x4725F0)") {
    CHECK(wtfFolderName("admin", true) == "ADMIN");
    CHECK(wtfFolderName("Dev Realm", false) == "Dev Realm");
    CHECK(wtfFolderName("a/b:c?", false) == "a_b_c_");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "vd_test_wtf_rename";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const std::filesystem::path account = root / "WTF" / "Account" / "ADMIN";
    const std::filesystem::path realm = account / "Dev Realm";
    std::filesystem::create_directories(realm / "Oldname");
    std::ofstream(realm / "Oldname" / "chat-cache.txt") << "VERSION 1";
    std::filesystem::create_directories(account / "Oldname"); // the older per-character folder
    std::filesystem::create_directories(realm / "Taken");

    CHECK(renameCharacterFolders(root, "admin", "Dev Realm", "Oldname", "Newname"));
    CHECK_FALSE(std::filesystem::exists(realm / "Oldname"));
    CHECK(std::filesystem::exists(realm / "Newname" / "chat-cache.txt"));
    CHECK(std::filesystem::is_directory(account / "Newname"));
    // Nothing to move: as it is. A name taken in one place leaves that folder, the other one moves.
    CHECK_FALSE(renameCharacterFolders(root, "admin", "Dev Realm", "Nobody", "Someone"));
    CHECK(renameCharacterFolders(root, "admin", "Dev Realm", "Newname", "Taken"));
    CHECK(std::filesystem::exists(realm / "Newname" / "chat-cache.txt"));
    CHECK(std::filesystem::is_directory(account / "Taken"));
    std::filesystem::remove_all(root, error);
}
