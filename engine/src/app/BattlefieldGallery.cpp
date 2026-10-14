#include "app/BattlefieldGallery.h"

#include "app/UnitMenuWindows.h"
#include "core/Log.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <chrono>
#include <cstdio>

namespace vd {
namespace {

using game::BattlefieldState;

// Placeholder descriptions, about as long as Map.dbc's (the game shows the client's, the same for both factions).
constexpr std::string_view kWarsongText = "A wooded gorge between Ashenvale Forest and the Barrens, Warsong "
                                          "Gulch never knows a quiet day. The Horde's lumber "
                                          "mill keeps eating into the forest, and the night elves "
                                          "of Silverwing Hold answer every felled tree with "
                                          "arrows. Each side keeps a flag in its base, and fighters "
                                          "of every race come to the gorge to carry the "
                                          "enemy's flag home.";
constexpr std::string_view kAlteracText = "High in the snowbound Alterac Mountains, Alterac Valley is held "
                                          "by the Frostwolf orcs on one side and by the "
                                          "Stormpike dwarves, who dig its mines for ore and old secrets, "
                                          "on the other. Towers, graveyards and bunkers "
                                          "line the road between their bases. Neither side will give up "
                                          "the valley, and both have called for every "
                                          "soldier they can find... the larger army may well win.";
constexpr std::string_view kArathiText = "Arathi Basin is a quiet farmland turned into a battlefield, "
                                         "with its farm, mill, mine, lumber yard and stables "
                                         "worth holding. The League of Arathor fights to take the "
                                         "land back for Stormwind, while the Defilers of the "
                                         "Forsaken want it for themselves. Whoever holds more of the "
                                         "five places gathers resources faster, and the "
                                         "first side to two thousand wins.";

struct Sample {
    std::string_view map;
    std::string_view description;
    u32 instances[6];
    u32 count;
    u32 queued;     // the instance queued for (0: First Available), ~0u none
    u32 confirm;    // the instance invited to, ~0u none
    i32 selected;   // the row selected at first
    bool groupJoin; // CanJoinBattlefieldAsGroup
    bool leader;
};
constexpr u32 kNone = ~0u;
constexpr Sample kSamples[] = {
    {"Warsong Gulch", kWarsongText, {1, 2, 4, 5, 7}, 5, 4, 7, 3, true, false},
    {"Alterac Valley", kAlteracText, {}, 0, 0, kNone, 0, false, true},
    {"Arathi Basin", kArathiText, {2, 3, 6}, 3, kNone, kNone, 0, true, true},
};

} // namespace

bool BattlefieldGallery::select(std::string_view page) {
    if (page == "battlefield") {
        m_page = Page::Frame;
    } else if (page == "battlefield2") {
        m_page = Page::AlteracValley;
    } else if (page == "battlefield3") {
        m_page = Page::Group;
    } else if (page == "bgbutton") {
        m_page = Page::Button;
    } else {
        return false;
    }
    return true;
}

void BattlefieldGallery::draw(DialogContext& c, u64 nowMs) {
    const auto start = std::chrono::steady_clock::now();
    if (m_page == Page::Button) {
        button(c, nowMs);
    } else {
        frame(c, nowMs);
    }
    const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_costSum += ms;
    m_costWorst = std::max(m_costWorst, ms);
    if (++m_costFrames == 600) {
        VD_LOG_INFO("UI gallery: battlefield %s %.3f ms avg, %.3f ms worst (600 frames)",
                    m_page == Page::Button ? "MiniMapBattlefieldFrame" : "BattlefieldFrame",
                    m_costSum / 600.0f, m_costWorst);
        m_costFrames = 0;
        m_costSum = m_costWorst = 0.0f;
    }
}

void BattlefieldGallery::frame(DialogContext& c, u64 nowMs) {
    (void)nowMs;
    const Sample& s = kSamples[m_page == Page::Frame ? 0 : m_page == Page::AlteracValley ? 1 : 2];
    if (m_selected < 0) {
        m_selected = s.selected;
    }
    if (m_tipRow == -2) {
        m_tipRow = m_page == Page::Group ? 0 : -1;
        m_tipAnchor = {c.safeArea.x + c.ui.px(40.0f), c.safeArea.y + c.ui.px(150.0f), c.ui.px(200.0f),
                       c.ui.px(30.0f)};
    }
    const u32 rows = s.count + 1;
    for (u32 i = 0; i < rows; ++i) {
        BattlefieldRowView& row = m_rows[i];
        const u32 instance = i == 0 ? 0 : s.instances[i - 1];
        if (i == 0) {
            row.text = text::get("FIRST_AVAILABLE");
        } else {
            const int n = std::snprintf(m_rowTexts[i].data(), m_rowTexts[i].size(), "%.*s %u",
                                        static_cast<int>(s.map.size()), s.map.data(), instance);
            row.text = {m_rowTexts[i].data(), static_cast<usize>(n > 0 ? n : 0)};
        }
        // BattlefieldFrame_Update's statuses: the queue's battle, or First Available with none running.
        row.status = {};
        if (s.queued != kNone &&
            ((i > 0 && instance == s.queued) || (i == 0 && s.queued == 0 && s.count == 0))) {
            row.status = text::get("BATTLEFIELD_QUEUE_STATUS");
        } else if (s.confirm != kNone && i > 0 && instance == s.confirm) {
            row.status = text::get("BATTLEFIELD_CONFIRM_STATUS");
        }
    }
    BattlefieldFrameView v;
    v.title = s.map;
    v.rows = {m_rows.data(), rows};
    v.selected = m_selected;
    v.description = s.description;
    v.groupJoinShown = s.groupJoin;
    v.groupJoinEnabled = s.leader;
    const BattlefieldFrameResult r = battlefieldFrame(c, v);
    if (r.select >= 0) {
        m_selected = r.select;
        VD_LOG_INFO("UI gallery: battlefield row %d (SetSelectedBattlefield)", r.select);
    }
    if (r.join || r.joinAsGroup) {
        VD_LOG_INFO("UI gallery: battlefield JoinBattlefield(%d%s), HideUIPanel", m_selected,
                    r.joinAsGroup ? ", 1" : "");
    }
    if (r.closed) {
        VD_LOG_INFO("UI gallery: battlefield closed (HideUIPanel: CloseBattlefield)");
    }
    if (r.rowTip >= 0 || r.groupTip) {
        const i32 tip = r.groupTip ? 100 : r.rowTip;
        m_tipRow = m_tipRow == tip ? -1 : tip;
        m_tipAnchor = r.tipAnchor;
    }
    if (m_tipRow == 100) {
        battlefieldTooltip(c, m_tipAnchor, text::get("BATTLEFIELD_GROUP_JOIN"),
                           text::get("NEWBIE_TOOLTIP_BATTLEFIELD_GROUP_JOIN"));
    } else if (m_tipRow >= 0 && static_cast<u32>(m_tipRow) < rows) {
        battlefieldTooltip(c, m_tipAnchor, m_rows[static_cast<u32>(m_tipRow)].text,
                           text::get(m_tipRow == 0 ? "NEWBIE_TOOLTIP_FIRST_AVAILABLE"
                                                   : "NEWBIE_TOOLTIP_ENTER_BATTLEGROUND"));
    }
}

void BattlefieldGallery::button(DialogContext& c, u64 nowMs) {
    (void)nowMs;
    // The cluster (Stormwind) with the button under the map's bottom left.
    const MinimapLayout layout = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    MinimapView view;
    view.zoneName = "Stormwind City";
    view.hasTime = true;
    drawMinimap(c.ui, c.draw, c.resources, layout, view, m_gesture);
    BattlefieldButtonView bv;
    const BattlefieldButtonResult r = battlefieldMinimapButton(c, layout, bv);
    if (r.tapped) {
        m_tooltip = !m_tooltip;
        VD_LOG_INFO("UI gallery: battlefield button tapped (the tooltip; while a battle is active: the "
                    "score, a second "
                    "tap the battle map)");
    }
    if (r.longPress) {
        m_menu = !m_menu;
    }
    // Warsong Gulch 3 running, Arathi Basin queued for 4 min 12 s (3 min on average), Alterac Valley 2
    // waiting.
    m_queues[0] = {BattlefieldState::Active, "Warsong Gulch 3", "Warsong Gulch", 0, 0, 0};
    m_queues[1] = {BattlefieldState::Queued, "Arathi Basin", "Arathi Basin", 180000, 252000, 0};
    m_queues[2] = {BattlefieldState::Confirm, "Alterac Valley 2", "Alterac Valley", 0, 0, 80000};
    if (m_tooltip) {
        battlefieldButtonTooltip(c, r.rect, battlefieldStatusText(m_tipText, m_queues));
    }
    if (m_menu) {
        DropDownMenuView mv;
        const ui::Rect anchor = battlefieldMenuAnchor(layout, bv.place);
        mv.x = anchor.x;
        mv.y = anchor.y;
        mv.level1 = {m_items.data(), battlefieldMenu(m_queues, m_items, m_actions, m_slots)};
        const DropDownMenuResult mr = dropDownMenu(c, "gallery.battlefieldMenu", mv);
        if (mr.level == 1 && mr.picked >= 0) {
            VD_LOG_INFO("UI gallery: battlefield menu: %.*s (queue %u)",
                        static_cast<int>(m_items[static_cast<u32>(mr.picked)].text.size()),
                        m_items[static_cast<u32>(mr.picked)].text.data(),
                        m_slots[static_cast<u32>(mr.picked)] + 1u);
            m_menu = false;
        } else if (mr.closed) {
            m_menu = false;
        }
    }
    // CONFIRM_BATTLEFIELD_ENTRY for Alterac Valley 2.
    std::array<PopupView, 4> popups{};
    popups[0].text = text::format(m_popupText, "CONFIRM_BATTLEFIELD_ENTRY", m_queues[2].name);
    popups[0].button1 = text::get("ENTER_BATTLE");
    popups[0].button2 = text::get("HIDE");
    const PopupTaps taps = popupStack(c, popups, m_popupHeights);
    if (taps.slot >= 0) {
        VD_LOG_INFO("UI gallery: CONFIRM_BATTLEFIELD_ENTRY button %u (%s)", taps.button,
                    taps.button == 1 ? "AcceptBattlefieldPort(3, 1)" : "hidden");
    }
}

} // namespace vd
