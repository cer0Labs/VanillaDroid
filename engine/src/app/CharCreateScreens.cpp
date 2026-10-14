#include "app/CharCreateScreens.h"

#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/SocialText.h"
#include "net/ResponseCodes.gen.h"
#include "net/SocialMessages.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility> // phase 18 (extras)

namespace vd {
namespace {

using net::ResponseCode;

// SetCharacterCreateFacing(-15): CharacterCreate_OnShow, CharacterRace_OnClick.
constexpr f32 kStartFacing = -15.0f;
constexpr u64 kDemoAnswerMs = 1000; // VD_CREATE_DEMO: the "server" answers a second later
constexpr ui::WidgetId kNameField = ui::widgetId("create.name"); // CharCreateWindows' name box

// ChrRaces / ChrClasses m_clientFileString, upper case as the Lua builds GlueStrings' keys from them
// (RACE_INFO_<race>, ABILITY_INFO_<race><n>, CLASS_<class>).
constexpr const char* kRaceFiles[] = {"",        "HUMAN",  "ORC",   "DWARF", "NIGHTELF",
                                      "SCOURGE", "TAUREN", "GNOME", "TROLL"};
constexpr const char* kClassFiles[] = {"", "WARRIOR", "PALADIN", "HUNTER",  "ROGUE", "PRIEST",
                                       "", "SHAMAN",  "MAGE",    "WARLOCK", "",      "DRUID"};

const char* raceFile(u8 race) {
    return race < std::size(kRaceFiles) ? kRaceFiles[race] : "";
}

const char* classFile(u8 cls) {
    return cls < std::size(kClassFiles) ? kClassFiles[cls] : "";
}

// A ResponseCode by its name or number (VD_CREATE_DEMO_RESULT).
u8 responseCode(const char* spec) {
    for (u32 code = 0; code < 256; ++code) {
        const char* name = net::responseCodeName(static_cast<u8>(code));
        if (name && std::strcmp(name, spec) == 0) {
            return static_cast<u8>(code);
        }
    }
    const int number = std::atoi(spec);
    return number > 0 && number < 256 ? static_cast<u8>(number)
                                      : static_cast<u8>(ResponseCode::CHAR_CREATE_SUCCESS);
}

} // namespace

void describeCharCreate(CharCreateView& v, std::string& abilities) {
    for (const u8 race : game::CharCreateData::kRaces) {
        v.raceNames[race] = game::raceName(race);
    }
    for (const u8 cls : game::CharCreateData::kClasses) {
        v.classNames[cls] = game::className(cls);
    }
    // SetCharacterRace: GetFactionForRace's name and FACTION_INFO_<faction>, RACE_INFO_<race>,
    // ABILITY_INFO_<race>1.. while they exist; SetCharacterClass: CLASS_<class>.
    v.alliance = game::CharCreateData::alliance(v.race);
    v.factionName = text::get(v.alliance ? "ALLIANCE" : "HORDE");
    v.factionText = text::get(v.alliance ? "FACTION_INFO_ALLIANCE" : "FACTION_INFO_HORDE");
    char key[48];
    std::snprintf(key, sizeof(key), "RACE_INFO_%s", raceFile(v.race));
    v.raceText = text::has(key) ? text::get(key) : std::string_view{};
    abilities.clear();
    for (u32 n = 1; n < 10; ++n) {
        std::snprintf(key, sizeof(key), "ABILITY_INFO_%s%u", raceFile(v.race), n);
        if (!text::has(key)) {
            break;
        }
        abilities.append(text::get(key));
        abilities.append("\n\n");
    }
    v.abilityText = abilities;
    std::snprintf(key, sizeof(key), "CLASS_%s", classFile(v.cls));
    v.classText = text::has(key) ? text::get(key) : std::string_view{};
}

void CharCreateScreens::init(const game::CharCreateData* create) {
    m_create = create && create->ready() ? create : nullptr;
    m_normalizedName.reserve(48);
    m_dialogText.reserve(256);
    m_abilityText.reserve(512);
    m_demo = VD_DEV_ENV("VD_CREATE_DEMO") != nullptr;
    const char* result = VD_DEV_ENV("VD_CREATE_DEMO_RESULT");
    m_demoResult = result ? responseCode(result) : static_cast<u8>(ResponseCode::CHAR_CREATE_SUCCESS);
    if (const char* race = VD_DEV_ENV("VD_CREATE_RACE")) {
        unsigned r = 0, gender = 0, cls = 0;
        if (std::sscanf(race, "%u:%u:%u", &r, &gender, &cls) >= 1) {
            m_startRace = static_cast<u8>(r);
            m_startGender = static_cast<u8>(gender & 1u);
            m_startClass = static_cast<u8>(cls);
        }
    }
    if (m_demo) {
        start();
    }
}

void CharCreateScreens::start() {
    m_active = true;
    m_waiting = false;
    m_dialog = CharCreateDialog::None;
    m_dialogText.clear();
    m_ui = CharCreateUiState{};
    m_facing = kStartFacing;
    m_appearance = game::CharAppearance{};
    if (m_create) {
        m_seed = m_seed * 1664525u + 1013904223u;
        m_create->reset(m_appearance, m_seed); // ResetCharCustomize: a random race, gender and look
        if (m_startRace != 0 && m_create->canBe(m_startRace, m_create->firstClass(m_startRace))) {
            m_create->selectRace(m_appearance, m_startRace, m_seed);
            m_create->selectGender(m_appearance, m_startGender, m_seed);
            if (m_create->canBe(m_appearance.race, m_startClass)) {
                m_appearance.cls = m_startClass;
            }
        }
    }
    m_name.wipe(); // CharacterCreateNameEdit:SetText("")
    syncLook();
}

f32 CharCreateScreens::facingRadians() const {
    return m_facing * 0.0174532925f; // SetCharacterCreateFacing: degrees * 0.0174533 (WoW.exe 0x47195F)
}

void CharCreateScreens::syncLook() {
    m_look = game::CharacterLook{};
    m_look.race = m_appearance.race;
    m_look.gender = m_appearance.gender;
    m_look.skin = m_appearance.skin;
    m_look.face = m_appearance.face;
    m_look.hairStyle = m_appearance.hairStyle;
    m_look.hairColor = m_appearance.hairColor;
    m_look.facialHair = m_appearance.facialHair;
    if (m_create) {
        // What the class starts in (CharStartOutfit), as the original's model wears it.
        std::array<u32, 19> displays;
        std::array<u8, 19> types;
        if (m_create->startOutfit(m_appearance.race, m_appearance.cls, m_appearance.gender, displays,
                                  types)) {
            for (u32 i = 0; i < game::kEquipSlots; ++i) {
                m_look.items[i] = displays[i];
                m_look.inventoryTypes[i] = types[i];
            }
        }
    }
}

void CharCreateScreens::fillView(const DialogContext& c, const ui::TextInput& text) {
    CharCreateView& v = m_view;
    const game::CharAppearance& a = m_appearance;
    v.race = a.race;
    v.cls = a.cls;
    v.gender = a.gender;
    v.races = game::CharCreateData::kRaces;
    const u32 classes = m_create ? m_create->classesOf(a.race, m_classes) : 0u;
    v.classes = {m_classes.data(), classes};
    describeCharCreate(v, m_abilityText);
    // The customization rows' labels by the race (horns, tusks, earrings...); frame 5 hides without facial
    // hair.
    const game::FeatureKind hair = m_create ? m_create->hairKind(a.race) : game::FeatureKind::HairStyle;
    const game::FeatureKind facial =
        m_create ? m_create->facialHairKind(a.race, a.gender) : game::FeatureKind::FacialHair;
    for (u32 i = 0; i < v.rows.size(); ++i) {
        const auto option = static_cast<game::AppearanceOption>(i);
        v.rows[i].label = text::get(game::customizationKey(option, hair, facial));
        v.rows[i].shown = i < 4 || !m_create || m_create->optionCount(a, option) > 0;
    }
    v.dialog = m_dialog;
    v.dialogText = m_dialogText;
    if (!m_create) {
        v.dialog = CharCreateDialog::Message;
        v.dialogText = text::get("VD_CREATE_NO_DATA");
    }
    // The screen keeps the place it has without the keyboard.
    if (!text.active() || m_layoutArea.w <= 0.0f) {
        m_layoutArea = c.safeArea;
    }
    v.layoutArea = m_layoutArea;
}

void CharCreateScreens::answer(u8 code, ui::TextInput& text) {
    m_waiting = false;
    if (code == static_cast<u8>(ResponseCode::CHAR_CREATE_SUCCESS)) {
        // 0x46C11E: the dialog closes and the character list shows again (GameSession asks for it).
        VD_LOG_INFO("Character created: %s", m_normalizedName.c_str());
        m_dialog = CharCreateDialog::None;
        m_active = false;
        m_demo = false;
        text.end();
        return;
    }
    m_dialog = CharCreateDialog::Message; // "OKAY" with the server's answer
    m_dialogText.assign(game::charResultText(code));
}

void CharCreateScreens::accept(GameSession& session, ui::TextInput& text) {
    // CreateCharacter (0x4711D0): the client's name check first, its error in an "OKAY" dialog.
    const u8 nameCheck = game::checkCharacterName(m_name.view(), m_normalizedName);
    if (nameCheck != static_cast<u8>(ResponseCode::CHAR_NAME_SUCCESS)) {
        m_dialog = CharCreateDialog::Message;
        m_dialogText.assign(game::charResultText(nameCheck));
        return;
    }
    if (!m_create || !m_create->valid(m_appearance)) {
        m_dialog = CharCreateDialog::Message;
        m_dialogText.assign(text::get("CHAR_CREATE_ERROR"));
        return;
    }
    net::CharCreateRequest request;
    request.name = m_normalizedName;
    request.race = m_appearance.race;
    request.cls = m_appearance.cls;
    request.gender = m_appearance.gender;
    request.skin = m_appearance.skin;
    request.face = m_appearance.face;
    request.hairStyle = m_appearance.hairStyle;
    request.hairColor = m_appearance.hairColor;
    request.facialHair = m_appearance.facialHair;
    if (m_demo) {
        m_demoAnswerMs = 0; // set on the next frame from the UI clock
    } else if (!session.createCharacter(request)) {
        m_dialog = CharCreateDialog::Message;
        m_dialogText.assign(text::get("CHAR_CREATE_ERROR"));
        return;
    }
    // 0x46B290: "CANCEL" with CHAR_CREATE_IN_PROGRESS until the answer.
    m_waiting = true;
    m_resultVersion = session.charResultVersion();
    m_dialog = CharCreateDialog::Creating;
    m_dialogText.assign(text::get("CHAR_CREATE_IN_PROGRESS"));
    text.end();
    VD_LOG_INFO("Creating %s (race %u class %u gender %u)", m_normalizedName.c_str(), request.race,
                request.cls, request.gender);
}

void CharCreateScreens::apply(GameSession& session, const CharCreateResult& r, ui::TextInput& text,
                              const UiSoundSink& sound) {
    m_facing = std::fmod(m_facing + r.turnDegrees, 360.0f); // degrees, wrapped
    if (r.scrollButton) {
        sound(UiSound::ChatScroll); // GlueScrollBarTemplate's buttons: PlaySound("UChatScrollButton")
    }
    if (r.dialogButton) {
        // GlueDialog_OnClick: the dialog goes; Cancel while creating stops waiting for the answer.
        m_dialog = CharCreateDialog::None;
        m_waiting = false;
        return;
    }
    if (!m_create) {
        if (r.back) {
            sound(UiSound::CharCreateCancel);
            m_active = false;
            m_demo = false;
        }
        return;
    }
    bool changed = false;
    if (r.race > 0) {
        // CharacterRace_OnClick: the sound always; the chosen race stays checked and nothing else happens.
        sound(UiSound::CharCreateClass);
        if (m_create->selectRace(m_appearance, static_cast<u8>(r.race), m_seed)) {
            m_facing = kStartFacing;
            changed = true;
        }
    }
    if (r.gender >= 0) {
        sound(UiSound::CharCreateClass); // SetCharacterGender, then PlaySound
        changed = m_create->selectGender(m_appearance, static_cast<u8>(r.gender), m_seed) || changed;
    }
    if (r.cls > 0) {
        sound(UiSound::CharCreateClass); // CharacterClass_OnClick
        if (m_create->canBe(m_appearance.race, static_cast<u8>(r.cls)) && m_appearance.cls != r.cls) {
            m_appearance.cls = static_cast<u8>(r.cls);
            changed = true;
        }
    }
    if (r.row >= 0) {
        sound(UiSound::CharCreateLook); // CharacterCustomization_Left / Right
        m_create->step(m_appearance, static_cast<game::AppearanceOption>(r.row), r.step);
        changed = true;
    }
    if (r.randomize) {
        sound(UiSound::CharCreateLook); // CharacterCreate_Randomize
        m_create->randomize(m_appearance, m_seed);
        changed = true;
    }
    if (r.randomName) {
        sound(UiSound::CharCreateLook);
        m_names.select(m_create->nameGen(), m_appearance.race, m_appearance.gender);
        const std::string_view random = m_names.generate(m_seed, m_randomName);
        if (!random.empty()) {
            m_name.set(random);
            if (text.active() && text.field() == kNameField) {
                text.setText(random);
            }
        }
    }
    if (changed) {
        syncLook();
    }
    if (r.accept) {
        sound(UiSound::CharCreateCreate); // CharacterCreate_Okay
        accept(session, text);
    }
    if (r.back) {
        sound(UiSound::CharCreateCancel); // CharacterCreate_Back: SetGlueScreen("charselect")
        m_active = false;
        m_demo = false;
        m_waiting = false;
        m_dialog = CharCreateDialog::None;
        text.end();
    }
}

void CharCreateScreens::draw(GameSession& session, DialogContext& c, ui::TextInput& text,
                             GlueSceneRequest& scene, const UiSoundSink& sound) {
    VD_PROFILE_ZONE("CharCreateScreens::draw");
    const GlueKey key = std::exchange(m_key, GlueKey::None); // phase 18 (extras)
    if (!m_demo && session.phase() != GameSession::Phase::CharacterSelect) {
        m_active = false; // disconnected: AccountLogin takes over
        text.end();
        return;
    }
    // The server's answer (SMSG_CHAR_CREATE), or the demo's.
    if (m_waiting && !m_demo && session.charResultVersion() != m_resultVersion) {
        answer(session.charResult(), text);
    } else if (m_waiting && m_demo) {
        if (m_demoAnswerMs == 0) {
            m_demoAnswerMs = c.ui.nowMs() + kDemoAnswerMs;
        } else if (c.ui.nowMs() >= m_demoAnswerMs) {
            answer(m_demoResult, text);
        }
    }
    if (!m_active) {
        return;
    }
    fillView(c, text);
    CharCreateResult r = characterCreateScreen(c, text, m_name, m_view, m_ui);
    if (key == GlueKey::Escape || key == GlueKey::Enter) {
        // Phase 18 (extras): GlueDialog_OnKeyDown while its dialog is up (one button: Okay, or Cancel while the
        // server is asked), else CharacterCreate_OnKeyDown: Escape is CharacterCreate_Back, Enter CharacterCreate_Okay.
        if (m_dialog != CharCreateDialog::None) {
            r.dialogButton = true;
        } else {
            r.back = r.back || key == GlueKey::Escape;
            r.accept = r.accept || key == GlueKey::Enter;
        }
    }
    apply(session, r, text, sound);
    if (!m_active) {
        return;
    }
    // SetBackgroundModel(CharacterCreate, fileString): the race's scene, the character standing in it.
    scene.background = glueBackgroundOfRace(m_appearance.race);
    scene.hasCharacter = true;
    scene.character = m_look;
    scene.characterFacing = facingRadians();
    scene.fade = 1.0f; // charselect -> charcreate does not fade (GlueScreenExit)
}

} // namespace vd
