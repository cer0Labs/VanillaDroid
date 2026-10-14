#include "app/PvpGallery.h"

#include "app/MessageHud.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/Chat.h"
#include "game/GameEvents.h"
#include "game/Honor.h"
#include "game/Notices.h"
#include "game/SocialText.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd {
namespace {

using ui::rgba;

constexpr u32 kChannelColor = rgba(255, 192, 192); // ChatTypeInfo CHANNEL (WoW.exe 0x805DD5)
constexpr u32 kWaveMs = 30000;                     // Spirit Heal Channel (22011): SpellDuration 9
constexpr i32 kHonorSamples[] = {37, -17, 82};     // the floating combat text's samples, in turn

game::GameEvent credit(game::HonorCredit kind, i32 honor, u32 rank, i32 side) {
    game::GameEvent e;
    e.kind = game::GameEventKind::HonorGained;
    e.guid = kind == game::HonorCredit::Awarded ? 0 : 1;
    e.id = rank;
    e.value = {honor, static_cast<i32>(kind), side};
    return e;
}

u32 honorColor() {
    const game::NoticeColor c = game::noticeColor(game::NoticeKind::HonorGain);
    return rgba(c.r, c.g, c.b);
}

} // namespace

bool PvpGallery::select(const char* name) {
    static constexpr const char* kNames[] = {"spirithealer", "skinned", "pvp"};
    for (u32 i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(name, kNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

std::vector<PvpGallery::Line> PvpGallery::sampleLines(bool honor) {
    std::vector<Line> out;
    char line[256];
    if (honor) {
        // CHAT_MSG_COMBAT_HONOR_GAIN as game::NoticeFeed words SMSG_PVP_CREDIT: an orc Grunt killed by an
        // Alliance player, a Horde innkeeper (a civilian), and a battleground's bonus honor.
        using game::HonorCredit;
        out.push_back({std::string(game::formatHonorLine(line, credit(HonorCredit::Honorable, 37, 6, 0),
                                                         "Grommash", false)),
                       honorColor()});
        out.push_back({std::string(game::formatHonorLine(line, credit(HonorCredit::Dishonorable, -17, 0, 0),
                                                         "Innkeeper Gryshka", false)),
                       honorColor()});
        out.push_back(
            {std::string(game::formatHonorLine(line, credit(HonorCredit::Awarded, 82, 0, 0), {}, false)),
             honorColor()});
        return out;
    }
    // The defense channels' lines as game::Honor adds them (no sender) and ChatFrame prints them: a guard
    // killed in Goldshire, the Eastern Plaguelands' tower (the server's broadcast text), and WorldDefense.
    const auto channelLine = [&](std::string_view channel, u32 number, std::string_view text) {
        game::ChatLine l;
        l.type = net::ChatType::Channel;
        l.channel = channel;
        l.text = text;
        Line made;
        game::appendChatLine(made.text, l, {}, number);
        made.color = kChannelColor;
        out.push_back(std::move(made));
    };
    channelLine("LocalDefense - Elwynn Forest", 2, text::format(line, "ZONE_UNDER_ATTACK", "Goldshire"));
    channelLine("LocalDefense - Eastern Plaguelands", 2,
                "|cffffff00The Alliance now holds Eastwall Tower!|r");
    channelLine("WorldDefense", 4, text::format(line, "ZONE_UNDER_ATTACK", "The Crossroads"));
    return out;
}

void PvpGallery::draw(DialogContext& c, u64 nowMs) {
    switch (m_page) {
    case Page::SpiritHealer: drawSpiritHealer(c, nowMs); break;
    case Page::Skinned: drawSkinned(c, {}, nowMs); break;
    case Page::Pvp:
        drawChat(c);
        drawHonorTexts(c, nowMs);
        break;
    }
}

void PvpGallery::drawSpiritHealer(DialogContext& c, u64 nowMs) {
    // AREA_SPIRIT_HEALER_IN_RANGE: StaticPopup_Show("AREA_SPIRIT_HEAL"), its time left the guide's 30 s wave.
    if (!m_stack.any()) {
        m_emptySinceMs = m_emptySinceMs == 0 ? nowMs : m_emptySinceMs;
        if (nowMs - m_emptySinceMs > 2000 || m_lastMs == 0) {
            m_emptySinceMs = 0;
            if (Popup* p = m_stack.show(PopupKind::AreaSpiritHeal, true)) {
                p->timeLeftMs = static_cast<i32>(kWaveMs - 1000); // whole seconds, as GetAreaSpiritHealerTime
            }
        }
    }
    m_stack.update(m_lastMs == 0 ? 0u : static_cast<u32>(nowMs - m_lastMs));
    m_lastMs = nowMs;
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        const Popup& p = m_stack.slots()[i];
        PopupView& v = m_views[i];
        v = PopupView{};
        if (!p.shown()) {
            continue;
        }
        const PopupInfo& info = popupInfo(p.kind);
        const u32 seconds = p.timeLeftMs <= 0 ? 0u : static_cast<u32>((p.timeLeftMs + 999) / 1000);
        v.text = seconds == 0 ? std::string_view(" ")
                              : text::format(m_popupText, info.text, seconds,
                                             text::get(seconds == 1 ? "SECONDS" : "SECONDS_P1"));
        v.button1 = text::get(info.button1);
    }
    const PopupTaps taps = popupStack(c, m_views, m_heights);
    if (taps.slot >= 0) {
        m_stack.click(static_cast<u32>(taps.slot), taps.button);
    }
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        VD_LOG_INFO("PvP gallery: AREA_SPIRIT_HEAL ended %u%s", static_cast<u32>(o.end),
                    o.end == PopupEnd::Accept ? " (CancelAreaSpiritHeal)" : "");
    }
}

void PvpGallery::drawSkinned(DialogContext& c, const ui::Rect& avoid, u64 nowMs) {
    if (!m_errorsFilled) { // never stamped (no update): the line stays
        m_errorsFilled = true;
        const game::NoticeColor red = game::noticeColor(game::NoticeKind::Error);
        char line[128];
        m_errors.add(text::format(line, "DEATH_CORPSE_SKINNED"), rgba(red.r, red.g, red.b));
    }
    errorsFrame(c, c.safeArea, m_errors, nowMs, 0.0f, avoid);
}

void PvpGallery::drawChat(DialogContext& c) {
    if (!m_chatBuilt) {
        m_chatBuilt = true;
        m_chatTexts = sampleLines(false);
        m_logTexts = sampleLines(true);
        for (u32 i = 0; i < m_chatTexts.size(); ++i) {
            m_chatLines.push_back({m_chatTexts[i].text, m_chatTexts[i].color, i, {}});
        }
        for (u32 i = 0; i < m_logTexts.size(); ++i) {
            m_logLines.push_back({m_logTexts[i].text, m_logTexts[i].color, 1000 + i, {}});
        }
        m_tabs = {{{text::get("GENERAL")}, {text::get("COMBAT_LOG")}}};
    }
    ChatFrameView v;
    v.lines = m_selected == 1 ? std::span<const ChatFrameLine>(m_logLines)
                              : std::span<const ChatFrameLine>(m_chatLines);
    v.tabs = m_tabs;
    v.selected = static_cast<u32>(m_selected);
    v.expanded = m_expanded;
    v.backgroundAlpha = 1.0f;
    v.header = text::get("CHAT_SAY_SEND");
    v.headerColor = ui::kWhite;
    v.hint = text::get("VD_SOCIAL_CHAT_HINT");
    const ui::Rect frame = chatFrameRect(c, v.expanded, {}, {}, false);
    const ChatFrameResult r = chatFrame(c, frame, v, m_chatState);
    if (r.toggle) {
        m_expanded = !m_expanded;
    }
    if (r.tab >= 0) {
        m_selected = r.tab;
    }
}

void PvpGallery::drawHonorTexts(DialogContext& c, u64 nowMs) {
    // Blizzard_CombatText's HONOR_GAINED (the option on, as a player who enabled the floating combat text
    // sees it).
    m_settings.showCombatText = true;
    m_settings.combatTextHonorGained = true;
    if (nowMs >= m_nextHonorMs) {
        m_nextHonorMs = nowMs + 2500;
        m_combatText.add(CombatTextScreens::Kind::HonorGained, m_settings, {},
                         kHonorSamples[m_honorIndex++ % std::size(kHonorSamples)], 0,
                         static_cast<u32>(nowMs));
    }
    m_combatText.draw(c, m_settings, static_cast<u32>(nowMs));
    // "HK: <rank>" rising over the player's head (WoW.exe 0x6072E0; app/PvpScreens puts it on the player):
    // here over where the character stands, in COMBAT_HONOR_GAIN's colour, as the HUD draws its floating
    // texts.
    char text[64];
    const std::string_view hk =
        game::formatHonorWorldText(text, credit(game::HonorCredit::Honorable, 37, 6, 0), false);
    ui::Gui g{c.ui, c.draw};
    const f32 t = static_cast<f32>(nowMs % 1500) / 1500.0f;
    ui::TextStyle style;
    style.font = ui::FontId::Skurri;
    style.size = g.px(18.0f) * (t < 0.1f ? 0.7f + 3.0f * t : 1.0f);
    style.color = ui::withAlpha(honorColor(), t < 0.7f ? 1.0f : 1.0f - (t - 0.7f) / 0.3f);
    style.outline = std::max(1.0f, g.px(1.2f));
    style.align = ui::Align::Center;
    const ui::Rect& s = c.safeArea;
    g.draw.text(s.centerX(), s.y + s.h * 0.42f - g.px(60.0f) * t, hk, style);
}

} // namespace vd
