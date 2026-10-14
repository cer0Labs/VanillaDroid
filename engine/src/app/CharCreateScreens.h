#pragma once

#include "app/CharCreateWindows.h"
#include "app/Dialogs.h"
#include "app/GlueKeys.h" // phase 18 (extras)
#include "app/GlueScene.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Appearance.h"
#include "game/CharCreate.h"
#include "ui/TextInput.h"

#include <array>
#include <string>

namespace vd {

class GameSession;

// The texts SetCharacterRace and SetCharacterClass show for v.race, v.gender and v.cls (GlueStrings by the
// races' and classes' file names): the faction's name and text, the race's text and its ABILITY_INFO lines
// (into `abilities`, each followed by a blank line), the class's text; and the races' and classes' names
// (ChrRaces, ChrClasses).
void describeCharCreate(CharCreateView& v, std::string& abilities);

// Phase 18 (create): the character creation screen as GlueXML's CharacterCreate.lua and WoW.exe's C side of
// its Lua API behave (CGlueMgr's customization; addresses in game/CharCreate.h), over the race's glue scene:
// it opens on a random race, gender and look with the race's first class (CharacterCreate_OnShow,
// ResetCharCustomize), facing -15 degrees; a race button gives its first class and a random look and faces
// -15 again (CharacterRace_OnClick); a gender button a random look (SetSelectedSex); the arrows cycle a
// customization, Randomize draws a whole look, the name's Randomize a NameGen name (game::NameGenerator).
// Accept checks the name as the client does before sending it (CreateCharacter, 0x4711D0: GlueDialog "OKAY"
// with the CHAR_NAME_* text), sends CMSG_CHAR_CREATE behind GlueDialog's "Creating character" / Cancel
// (0x46B290) and either goes back to the character list on CHAR_CREATE_SUCCESS (0x46C0F8: the list is asked
// for again) or shows the server's answer in an "OKAY" dialog. Back goes back to the list. Its sounds are
// CharacterCreate.lua's (gs* SoundEntries). The scene it asks for (GlueSceneRequest): the race's background
// (SetBackgroundModel), the character in its look and its class's starting outfit (CharStartOutfit), turned
// by the facing (SetCharacterCreateFacing).
//
// Development: VD_CREATE_DEMO=1 opens it without a server (GameScreens' login screen); its Accept is answered
// after a second by VD_CREATE_DEMO_RESULT (a ResponseCode name such as CHAR_CREATE_NAME_IN_USE, or its
// number; success when unset). VD_CREATE_RACE=<race>[:<gender>[:<class>]] (ChrRaces / ChrClasses ids, gender
// 0 male) opens it on that choice.
class CharCreateScreens {
public:
    // `create` may be null (no db.vdpk): the screen then only says so.
    void init(const game::CharCreateData* create);
    // CharacterCreate shown (CharacterSelect's Create New Character).
    void start();
    bool active() const { return m_active; }
    // Phase 18 (extras): a hardware key (CharacterCreate_OnKeyDown: Escape is Back, Enter is Okay; GlueDialog's own
    // while one is up), applied on the next frame.
    void key(GlueKey key) { m_key = key; }
    // The screen, while active(): the server's answer, the view, the player's taps. Fills `scene` with what
    // it shows.
    void draw(GameSession& session, DialogContext& c, ui::TextInput& text, GlueSceneRequest& scene,
              const UiSoundSink& sound);
    // The character being made (null when not active) and its facing in radians (SetCharacterCreateFacing's
    // degrees, -15 when the screen or a race opens; positive turns it counterclockwise seen from above).
    const game::CharacterLook* look() const { return m_active ? &m_look : nullptr; }
    f32 facingRadians() const;
    const game::CharAppearance& appearance() const { return m_appearance; }

private:
    void syncLook();
    void fillView(const DialogContext& c, const ui::TextInput& text);
    void answer(u8 code, ui::TextInput& text);
    void apply(GameSession& session, const CharCreateResult& r, ui::TextInput& text,
               const UiSoundSink& sound);
    void accept(GameSession& session, ui::TextInput& text);

    const game::CharCreateData* m_create = nullptr;
    game::NameGenerator m_names;
    bool m_active = false;
    GlueKey m_key = GlueKey::None; // phase 18 (extras)
    bool m_demo = false;                                     // VD_CREATE_DEMO: no server
    u8 m_demoResult = 0;                                     // VD_CREATE_DEMO_RESULT
    u64 m_demoAnswerMs = 0;                                  // when the demo's "server" answers
    u8 m_startRace = 0, m_startGender = 0, m_startClass = 0; // VD_CREATE_RACE
    game::CharAppearance m_appearance;
    game::CharacterLook m_look;
    f32 m_facing = -15.0f; // degrees
    u32 m_seed = 0x5EED1234u;
    FormText<48> m_name;
    std::string m_normalizedName;
    std::array<char, 16> m_randomName{};
    // CMSG_CHAR_CREATE sent and not answered; GlueDialog.
    bool m_waiting = false;
    u32 m_resultVersion = 0;
    CharCreateDialog m_dialog = CharCreateDialog::None;
    std::string m_dialogText;
    // The view.
    CharCreateView m_view;
    CharCreateUiState m_ui;
    std::array<u8, 8> m_classes{};
    u8 m_abilityRace = 0;
    std::string m_abilityText; // the race's ABILITY_INFO_* lines, "\n\n" between them (SetCharacterRace)
    ui::Rect m_layoutArea;     // the safe area last seen without the keyboard
};

} // namespace vd
