#include "app/RaidAlertScreens.h"

#include "app/GameSession.h"
#include "app/MessageHud.h"
#include "app/Portraits.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/Gameplay.h"
#include "game/SocialText.h"

#include "net/UpdateFields.gen.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace vd {
namespace {

using net::ChatType;

// RaidWarningFrame / RaidBossEmoteFrame: MessageFrame's defaults (displayDuration 10, fadeDuration 3:
// WoW.exe's CSimpleMessageFrame constructor, 0x78572A), insertMode TOP; 40 units of GameFontNormalHuge hold
// two lines.
constexpr ui::MessageFrame::Config kFrameConfig{4, 10000, 3000, true};

// The races in the stand-in portraits' file names (ChrRaces m_clientFileString: not texts).
constexpr const char* kRaceFiles[] = {"Human",   "Orc",    "Dwarf", "NightElf",
                                      "Scourge", "Tauren", "Gnome", "Troll"};

u64 racePortrait(u32 race, u32 gender) {
    char path[96];
    std::snprintf(path, sizeof(path), "Interface\\CharacterFrame\\TemporaryPortrait-%s-%s.blp",
                  gender == 1 ? "Female" : "Male", kRaceFiles[(race >= 1 && race <= 8) ? race - 1 : 0]);
    return assetPathHash(path);
}

// Development samples (English, as the server sends its texts).
constexpr const char* kDemoWarnings[] = {"Everyone out of the fire!",
                                         "Healers on the main tank, kill the adds"};
constexpr const char* kDemoEmotes[] = {"Magmadar goes into a killing frenzy!",
                                       "Lucifron's Flamewaker Protectors are freed!"};
constexpr u32 kDemoEveryMs = 4000;
constexpr u32 kDemoReadyCheckGapMs = 3000; // the demo's frame shows again this long after it went

} // namespace

RaidAlertScreens::RaidAlertScreens() : m_warnings(kFrameConfig), m_emotes(kFrameConfig) {
    m_scratch.reserve(512);
    m_readyCheckDemo = VD_DEV_ENV("VD_READYCHECK_DEMO") != nullptr;
    m_readyCheckSelf = VD_DEV_ENV("VD_READYCHECK_SELF") != nullptr;
    m_raidWarningDemo = VD_DEV_ENV("VD_RAIDWARNING_DEMO") != nullptr;
}

void RaidAlertScreens::leaveWorld() {
    m_warnings.clear();
    m_emotes.clear();
    m_leader = 0;
    m_cursorsPlayer = 0; // the rings read again from the next world's start
}

void RaidAlertScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("RaidAlertScreens::update");
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) { // a new character: its lines and events from now on
        m_cursorsPlayer = session.playerGuid();
        m_chatCursor = play.chat().end();
        m_eventCursor = play.events().end();
        m_warnings.clear();
        m_emotes.clear();
    }
    play.readyCheck().setSelfPrompt(m_readyCheckSelf); // VD_READYCHECK_SELF (development)
    readChat(session, nowMs);
    readEvents(session);
}

void RaidAlertScreens::readChat(GameSession& session, u32 nowMs) {
    (void)nowMs;
    game::Gameplay& play = session.play();
    while (const game::ChatLine* line = play.chat().next(m_chatCursor)) {
        if (line->source != game::ChatSource::Message) {
            continue;
        }
        if (line->type == ChatType::RaidWarning) {
            m_warnings.add(line->text,
                           kRaidWarningColor); // RaidWarningFrame_OnEvent: arg1, the message alone
        } else if (line->type == ChatType::RaidBossEmote) {
            // ChatFrame_OnEvent: format(CHAT_RAID_BOSS_EMOTE_GET .. arg1, arg2): the creature in the text's
            // %s.
            m_scratch.clear();
            game::appendChatLine(m_scratch, *line, play.senderName(*line), 0);
            m_emotes.add(m_scratch, kRaidBossEmoteColor);
        }
    }
}

void RaidAlertScreens::readEvents(GameSession& session) {
    game::Gameplay& play = session.play();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind == game::GameEventKind::ReadyCheck) {
            showReadyCheck(session, e->guid);
            m_uiSound(UiSound::ReadyCheck); // ShowReadyCheck: PlaySound("ReadyCheck")
        }
    }
}

void RaidAlertScreens::showReadyCheck(GameSession& session, u64 leader) {
    // ShowReadyCheck: the raid member of rank 2 (GetRaidRosterInfo), its name in READY_CHECK_MESSAGE.
    const game::Gameplay& play = session.play();
    m_leader = leader;
    std::string_view name;
    u8 race = 0, gender = 0;
    if (const std::string* known = play.queries().findPlayerName(leader)) {
        name = *known;
    }
    if (const game::PlayerInfo* info = play.queries().findPlayerInfo(leader)) {
        race = info->race;
        gender = info->gender;
    }
    for (const game::GroupMember& m : play.social().group().members) {
        if (m.guid == leader && name.empty()) {
            name = m.name;
        }
    }
    if (const game::Entity* unit = session.entities().find(leader)) { // in sight: its own fields
        race = static_cast<u8>(unit->byteValue(net::uf::UNIT_FIELD_BYTES_0, 0));
        gender = static_cast<u8>(unit->byteValue(net::uf::UNIT_FIELD_BYTES_0, 2));
    }
    text::format(m_text, "READY_CHECK_MESSAGE", name.empty() ? std::string_view("...") : name);
    m_fallback = racePortrait(race, gender);
}

void RaidAlertScreens::drawFrames(DialogContext& c, const ui::Rect& area, f32 below, const ui::Rect& avoid,
                                  u32 nowMs) {
    VD_PROFILE_ZONE("RaidAlertScreens::drawFrames");
    m_warnings.update(nowMs);
    m_emotes.update(nowMs);
    raidWarningFrames(c, area, m_warnings, m_emotes, nowMs, below, avoid);
}

void RaidAlertScreens::drawReadyCheck(GameSession& session, DialogContext& c, const ui::Rect& area, f32 below,
                                      const ui::Rect& buttons, u32 nowMs) {
    VD_PROFILE_ZONE("RaidAlertScreens::drawReadyCheck");
    game::ReadyCheck& check = session.play().readyCheck();
    if (!check.prompting()) {
        return;
    }
    if (m_leader != check.promptLeader() || m_text[0] == 0) {
        showReadyCheck(session, check.promptLeader()); // a frame shown before this screen saw its event
    }
    ReadyCheckView view;
    view.text = m_text.data();
    view.portrait = m_portraits && m_leader != 0 ? m_portraits->unit(m_leader) : TextureHandle{};
    view.portraitFallback = m_fallback;
    switch (readyCheckFrame(c, view, buttons, raidWarningFramesBottom(c, area, below) + c.ui.px(8.0f))) {
    case ReadyCheckChoice::Yes: check.answer(true, nowMs); break; // ConfirmReadyCheck(1)
    case ReadyCheckChoice::No: check.answer(false, nowMs); break; // ConfirmReadyCheck(): 0
    default: break;
    }
}

void RaidAlertScreens::demoUpdate(u32 nowMs) {
    if (m_raidWarningDemo && static_cast<i32>(nowMs - m_raidWarningDemoAtMs) >= 0) {
        const u32 step = m_raidWarningDemoStep++;
        m_warnings.add(kDemoWarnings[step % std::size(kDemoWarnings)], kRaidWarningColor);
        m_emotes.add(kDemoEmotes[step % std::size(kDemoEmotes)], kRaidBossEmoteColor);
        m_raidWarningDemoAtMs = nowMs + kDemoEveryMs;
    }
    if (m_readyCheckDemo && !m_readyCheckDemoShown && static_cast<i32>(nowMs - m_readyCheckDemoAtMs) >= 0) {
        m_readyCheckDemoShown = true;
        m_readyCheckDemoEndMs = nowMs + game::ReadyCheck::kDurationMs;
        text::format(m_text, "READY_CHECK_MESSAGE", std::string_view("Aeryn"));
        m_fallback = racePortrait(1, 1);
        m_uiSound(UiSound::ReadyCheck);
    }
}

void RaidAlertScreens::drawOffline(DialogContext& c, const ui::Rect& area, const ui::Rect& buttons,
                                   u32 nowMs) {
    if (!m_readyCheckDemo && !m_raidWarningDemo) {
        return;
    }
    VD_PROFILE_ZONE("RaidAlertScreens::drawOffline");
    demoUpdate(nowMs);
    if (m_readyCheckDemoShown) {
        ReadyCheckView view;
        view.text = m_text.data();
        view.portraitFallback = m_fallback;
        const ReadyCheckChoice choice =
            readyCheckFrame(c, view, buttons, raidWarningFramesBottom(c, area) + c.ui.px(8.0f));
        const bool expired = static_cast<i32>(nowMs - m_readyCheckDemoEndMs) >= 0;
        if (choice != ReadyCheckChoice::None || expired) {
            VD_LOG_INFO("Ready check demo: %s", choice == ReadyCheckChoice::Yes ? "Yes (ConfirmReadyCheck(1))"
                                                : choice == ReadyCheckChoice::No
                                                    ? "No (ConfirmReadyCheck(): 0)"
                                                    : "timed out (READY_CHECK_YOU_WERE_AFK)");
            m_readyCheckDemoShown = false;
            m_readyCheckDemoAtMs = nowMs + kDemoReadyCheckGapMs;
        }
    }
    m_warnings.update(nowMs);
    m_emotes.update(nowMs);
    raidWarningFrames(c, area, m_warnings, m_emotes, nowMs);
}

} // namespace vd
