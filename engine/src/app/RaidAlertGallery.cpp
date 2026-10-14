#include "app/RaidAlertGallery.h"

#include "app/MessageHud.h"
#include "app/PopupWindows.h"
#include "app/RaidAlertScreens.h"
#include "app/RaidAlertWindows.h"
#include "app/UnitTooltip.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/ItemTooltip.h"
#include "game/Notices.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <chrono>

namespace vd {
namespace {

using ui::rgba;

u32 colorOf(game::NoticeKind kind) {
    const game::NoticeColor c = game::noticeColor(kind);
    return rgba(c.r, c.g, c.b);
}

constexpr u64 kAerynPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Female-Human.blp");

} // namespace

void RaidAlertGallery::build(Page page) {
    using game::NoticeKind;
    m_built = page;
    m_ready = true;
    m_texts.clear();
    m_lines.clear();
    m_errors.clear();
    m_warnings.clear();
    m_emotes.clear();
    char line[512];
    const u32 system = colorOf(NoticeKind::System);
    const auto add = [&](u32 color, std::string_view text) {
        m_texts.emplace_back(text);
        m_lines.push_back({{}, color});
    };
    switch (page) {
    case Page::ReadyCheck: {
        // A member's lines (the leader Aeryn asks; this player let it run out), then the leader's own over a
        // full raid.
        add(system, text::format(line, "ERR_RAID_LEADER_READY_CHECK_START_S", "Aeryn"));
        add(system, text::format(line, "READY_CHECK_YOU_WERE_AFK"));
        add(system, text::format(line, "READY_CHECK_START"));
        add(system, text::format(line, "RAID_MEMBER_NOT_READY", "Brakk"));
        add(system, text::format(line, "RAID_MEMBER_NOT_READY", "Tolvir"));
        add(system, text::format(line, "RAID_MEMBERS_AFK", "Gorlash, Mirelle, Thandor, Vashka, Zul'zin"));
        add(system, text::format(line, "READY_CHECK_START"));
        add(system, text::format(line, "READY_CHECK_NO_AFK"));
        m_errors.add(text::format(line, "ERR_READY_CHECK_IN_PROGRESS"), colorOf(NoticeKind::Error));
        text::format(line, "READY_CHECK_MESSAGE", "Aeryn");
        m_readyText = line;
        break;
    }
    case Page::RaidWarning: {
        const u32 warning = RaidAlertScreens::kRaidWarningColor;
        const char* warnings[] = {"Healers on the main tank, kill the adds", "Everyone out of the fire!"};
        const char* emotes[] = {"Lucifron's guards break their chains!", "Magmadar flies into a blind rage!"};
        for (u32 i = 0; i < 2; ++i) {
            std::string chatLine(text::format(line, "CHAT_RAID_WARNING_GET", "|Hplayer:Aeryn|h[Aeryn]|h"));
            chatLine.append(warnings[i]);
            add(warning, chatLine);
            add(RaidAlertScreens::kRaidBossEmoteColor, emotes[i]); // CHAT_RAID_BOSS_EMOTE_GET is empty
            m_warnings.add(warnings[i], warning);
            m_emotes.add(emotes[i], RaidAlertScreens::kRaidBossEmoteColor);
        }
        m_errors.add(text::format(line, "ERR_RAID_GROUP_ONLY"), colorOf(NoticeKind::Error));
        break;
    }
    case Page::MeetingStone: {
        add(system, text::format(line, "ERR_MEETING_STONE_IN_QUEUE_S", "Wailing Caverns"));
        add(system, text::format(line, "ERR_MEETING_STONE_IN_PROGRESS"));
        add(system, text::format(line, "ERR_MEETING_STONE_MEMBER_ADDED_S", "Brakk"));
        add(system, text::format(line, "ERR_MEETING_STONE_OTHER_MEMBER_LEFT"));
        add(system, text::format(line, "ERR_MEETING_STONE_SUCCESS"));
        add(system, text::format(line, "ERR_MEETING_STONE_LEFT_QUEUE_S", "Wailing Caverns"));
        m_errors.add(text::format(line, "ERR_MEETING_STONE_INVALID_LEVEL"), colorOf(NoticeKind::Error));
        m_errors.add(text::format(line, "ERR_MEETING_STONE_MUST_BE_LEADER"), colorOf(NoticeKind::Error));
        break;
    }
    }
    for (u32 i = 0; i < m_texts.size(); ++i) {
        m_lines[i] = {m_texts[i], m_lines[i].color, i, {}};
    }
    m_tabs = {{{text::get("GENERAL")}}};
}

void RaidAlertGallery::chat(DialogContext& c) {
    ChatFrameView v;
    v.lines = m_lines;
    v.tabs = m_tabs;
    v.expanded = true;
    v.backgroundAlpha = 1.0f;
    v.header = text::get("CHAT_SAY_SEND");
    v.headerColor = ui::kWhite;
    v.hint = text::get("VD_SOCIAL_CHAT_HINT");
    const ui::Rect frame = chatFrameRect(c, true, {}, {}, false);
    chatFrame(c, frame, v, m_chatState);
}

void RaidAlertGallery::draw(DialogContext& c, Page page, u64 nowMs) {
    VD_PROFILE_ZONE("RaidAlertGallery::draw");
    if (!m_ready || m_built != page) {
        build(page);
    }
    chat(c);
    // The frames' lines are never stamped here (no update()): they stay.
    const ui::Rect chatBounds = chatFrameBounds(c, chatFrameRect(c, true, {}, {}, false), true);
    errorsFrame(c, c.safeArea, m_errors, nowMs, 0.0f, chatBounds);
    const auto start =
        std::chrono::steady_clock::now(); // the page's own frames, timed (the CPU cost per frame)
    switch (page) {
    case Page::ReadyCheck: {
        ReadyCheckView view;
        view.text = m_readyText;
        view.portraitFallback = kAerynPortrait;
        switch (readyCheckFrame(c, view, {}, raidWarningFramesBottom(c, c.safeArea) + c.ui.px(8.0f))) {
        case ReadyCheckChoice::Yes: VD_LOG_INFO("UI gallery: ready check Yes (ConfirmReadyCheck(1))"); break;
        case ReadyCheckChoice::No: VD_LOG_INFO("UI gallery: ready check No (ConfirmReadyCheck(): 0)"); break;
        default: break;
        }
        break;
    }
    case Page::RaidWarning:
        raidWarningFrames(c, c.safeArea, m_warnings, m_emotes, nowMs, 0.0f, chatBounds);
        break;
    case Page::MeetingStone: {
        // The cluster with the letter and the meeting stone's button under it, the button's tooltip.
        const MinimapLayout layout = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
        MinimapView view;
        view.zoneName = "The Barrens";
        view.hasTime = true;
        view.newMail = true;
        view.meetingStone = true;
        const MinimapResult r = drawMinimap(c.ui, c.draw, c.resources, layout, view, m_gesture);
        if (r.tapped == MinimapTip::MeetingStone) {
            m_tooltip = !m_tooltip;
        }
        if (r.meetingStoneClicked) {
            VD_LOG_INFO("UI gallery: meeting stone button: CONFIRM_LEAVE_QUEUE");
        }
        if (m_tooltip) {
            char tip[128];
            const MinimapTooltipLine lines[] = {
                {text::format(tip, "MEETINGSTONE_TOOLTIP", "Wailing Caverns"), ui::kWhite}};
            drawMinimapTooltip(c.ui, c.draw, layout.meetingStone(), lines, c.safeArea);
        }
        // The stone itself held in the world (app/ObjectTooltip): its name, its zone and its levels.
        {
            ui::Gui g{c.ui, c.draw};
            char level[64];
            const game::TooltipLine stone[] = {
                {"Meeting Stone", {}, game::kTooltipWhite},
                {"Wailing Caverns", {}, game::kTooltipGold},
                {text::format(level, "MEETINGSTONE_LEVEL", 15, 25), {}, game::kTooltipGold}};
            HudTooltipView tip;
            tip.lines = stone;
            const HudTooltipSize size = measureHudTooltip(g, tip, g.px(270.0f));
            hudTooltip(g, ui::widgetId("gallery.objectTooltip"),
                       defaultTooltipAnchor(g, c.safeArea, {}, c.safeArea.y + g.unit(200.0f), size), tip);
        }
        // CONFIRM_LEAVE_QUEUE as StaticPopup1.
        std::array<PopupView, 4> popups{};
        popups[0].text = text::get("CONFIRM_LEAVE_QUEUE");
        popups[0].button1 = text::get("ACCEPT");
        popups[0].button2 = text::get("CANCEL");
        const PopupTaps taps = popupStack(c, popups, m_popupHeights);
        if (taps.slot >= 0) {
            VD_LOG_INFO("UI gallery: CONFIRM_LEAVE_QUEUE button %u (%s)", taps.button,
                        taps.button == 1 ? "CancelMeetingStoneRequest" : "cancel");
        }
        break;
    }
    }
    const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_costSum += ms;
    m_costWorst = std::max(m_costWorst, ms);
    if (++m_costFrames == 600) {
        VD_LOG_INFO("UI gallery: %s frames %.3f ms avg, %.3f ms worst (600 frames)",
                    page == Page::ReadyCheck    ? "ReadyCheckFrame"
                    : page == Page::RaidWarning ? "raid warning"
                                                : "meeting stone",
                    m_costSum / 600.0f, m_costWorst);
        m_costFrames = 0;
        m_costSum = m_costWorst = 0.0f;
    }
}

} // namespace vd
