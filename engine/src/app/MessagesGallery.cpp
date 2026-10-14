#include "app/MessagesGallery.h"

#include "core/Log.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/Notices.h"
#include "game/SocialText.h"
#include "net/MirrorTimerMessages.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::rgba;

u32 colorOf(game::NoticeKind kind) {
    const game::NoticeColor c = game::noticeColor(kind);
    return rgba(c.r, c.g, c.b);
}

} // namespace

void MessagesGallery::build() {
    using game::NoticeKind;
    m_built = true;
    char line[256];
    char money[64];
    char item[128];
    const auto chat = [&](NoticeKind kind, std::string_view text) {
        m_chatTexts.emplace_back(text);
        m_chatLines.push_back({{}, colorOf(kind)});
    };
    const auto log = [&](NoticeKind kind, std::string_view text) {
        m_logTexts.emplace_back(text);
        m_logLines.push_back({{}, colorOf(kind)});
    };
    const auto link = [&](u32 entry, u8 quality, std::string_view name) {
        return game::formatItemLink(item, entry, game::itemQualityColor(quality), name);
    };
    // The General tab, as a session in Northshire fills it: chat from the server, then the client's own
    // lines (items linked as the client links them).
    m_chatTexts.emplace_back(text::format(line, "CHAT_SAY_GET", "|Hplayer:Arthel|h[Arthel]|h"));
    m_chatTexts.back().append("almost got the 10 tails");
    m_chatLines.push_back({{}, ui::kWhite});
    chat(NoticeKind::System, text::format(line, "ERR_QUEST_ACCEPTED_S", "Kobold Camp Cleanup"));
    chat(NoticeKind::Loot, text::format(line, "LOOT_ITEM_SELF_MULTIPLE", link(2589, 1, "Linen Cloth"), 2));
    chat(NoticeKind::Loot, text::format(line, "LOOT_ITEM_SELF", link(2970, 2, "Wolfskin Bracers")));
    chat(NoticeKind::Skill, text::format(line, "ERR_SKILL_UP_SI", "Swords", 12));
    chat(NoticeKind::Loot, text::format(line, "LOOT_ITEM", "Aeryn", link(4865, 0, "Ruined Pelt")));
    chat(NoticeKind::System, text::format(line, "ERR_QUEST_COMPLETE_S", "Kobold Camp Cleanup"));
    chat(NoticeKind::System, text::format(line, "ERR_QUEST_REWARD_EXP_I", 170));
    chat(NoticeKind::System, text::format(line, "ERR_QUEST_REWARD_MONEY_S", game::moneyText(money, 150)));
    chat(NoticeKind::System, text::format(line, "LEVEL_UP", 6));
    chat(NoticeKind::System, text::format(line, "LEVEL_UP_HEALTH_MANA", 15, 20));
    chat(NoticeKind::System, text::format(line, "LEVEL_UP_STAT", text::get("SPELL_STAT2_NAME"), 1));
    chat(NoticeKind::System, text::format(line, "LEVEL_UP_STAT", text::get("SPELL_STAT3_NAME"), 2));
    chat(NoticeKind::System, text::format(line, "LEVEL_UP_STAT", text::get("SPELL_STAT4_NAME"), 1));
    chat(NoticeKind::System, text::format(line, "ERR_LEARN_SPELL_S", "Frostbolt"));
    chat(NoticeKind::Loot, text::format(line, "LOOT_ITEM_PUSHED_SELF", link(4540, 1, "Tough Hunk of Bread")));
    m_chatLinks.resize(m_chatTexts.size());
    for (u32 i = 0; i < m_chatTexts.size(); ++i) {
        u32 links = 0;
        game::ChatLink found;
        for (usize from = 0; links < 4 && game::nextChatLink(m_chatTexts[i], from, found); from = found.end) {
            m_chatLinks[i][links++] = {static_cast<u16>(found.begin), static_cast<u16>(found.end)};
        }
        m_chatLines[i] = {m_chatTexts[i], m_chatLines[i].color, i, {m_chatLinks[i].data(), links}};
    }
    // The combat log: experience, reputation and money (ChatFrame2 in 1.12).
    log(NoticeKind::XpGain,
        text::format(line, "COMBATLOG_XPGAIN_EXHAUSTION1", "Young Wolf", 52, "+26", "Rested"));
    log(NoticeKind::Money, text::format(line, "YOU_LOOT_MONEY", game::moneyText(money, 7)));
    log(NoticeKind::XpGain, text::format(line, "COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", 170));
    log(NoticeKind::FactionChange, text::format(line, "FACTION_STANDING_INCREASED", "Stormwind", 75));
    log(NoticeKind::FactionChange,
        text::format(line, "FACTION_STANDING_CHANGED", text::get("FACTION_STANDING_LABEL5"), "Stormwind"));
    log(NoticeKind::Money, text::format(line, "LOOT_MONEY_SPLIT", game::moneyText(money, 10203)));
    for (u32 i = 0; i < m_logTexts.size(); ++i) {
        m_logLines[i].text = m_logTexts[i];
        m_logLines[i].key = 1000 + i;
    }
    m_tabs = {{{text::get("GENERAL")}, {text::get("COMBAT_LOG")}}};
    // The UIErrorsFrame: never stamped here (update() is not called), so the lines stay.
    m_errors.add(text::format(line, "ERR_QUEST_ADD_KILL_SII", "Kobold Vermin", 7, 10),
                 colorOf(NoticeKind::Info));
    m_errors.add(text::format(line, "SPELL_FAILED_OUT_OF_RANGE"), colorOf(NoticeKind::Error));
    m_errors.add(text::format(line, "ERR_QUEST_OBJECTIVE_COMPLETE_S", "Kobold Camp Cleanup"),
                 colorOf(NoticeKind::Info));
}

void MessagesGallery::draw(DialogContext& c, bool combatLog, u64 nowMs) {
    if (!m_built) {
        build();
    }
    // The three bars: breath running out, fatigue, feign death (its label is the spell's name: 1.12 has
    // none).
    const f32 t = static_cast<f32>(nowMs % 20000) / 20000.0f;
    m_bars[0] = {true, text::get("BREATH_LABEL"), 1.0f - t, mirrorTimerColor(net::kMirrorTimerBreath)};
    m_bars[1] = {true, text::get("EXHAUSTION_LABEL"), 0.4f, mirrorTimerColor(net::kMirrorTimerFatigue)};
    m_bars[2] = {true, "Feign Death", 0.85f, mirrorTimerColor(net::kMirrorTimerFeignDeath)};
    // The chat where SocialScreens puts it: open over the lower left with General, or at rest on the Combat Log
    // (phase 20: no tabs at rest); then the bars and the UIErrorsFrame clear of it.
    if (m_selected < 0) {
        m_selected = combatLog ? 1 : 0;
        m_expanded = combatLog ? 0 : 1;
    }
    ChatFrameView v;
    v.lines = m_selected == 1 ? std::span<const ChatFrameLine>(m_logLines) : std::span<const ChatFrameLine>(m_chatLines);
    v.tabs = m_tabs;
    v.selected = static_cast<u32>(m_selected);
    v.expanded = m_expanded == 1;
    v.backgroundAlpha = 1.0f;
    v.header = text::get("CHAT_SAY_SEND");
    v.headerColor = ui::kWhite;
    v.hint = text::get("VD_SOCIAL_CHAT_HINT");
    const ui::Rect frame = chatFrameRect(c, v.expanded, {}, {}, false);
    const ChatFrameResult r = chatFrame(c, frame, v, m_chatState);
    if (r.toggle) {
        m_expanded = 1 - m_expanded;
    }
    if (r.tab >= 0) {
        m_selected = r.tab;
    }
    if (r.linkLine >= 0) {
        VD_LOG_INFO("UI gallery: chat link at %u of line %d", r.linkBegin, r.linkLine);
    }
    const ui::Rect bounds = chatFrameBounds(c, frame, v.expanded);
    errorsFrame(c, c.safeArea, m_errors, nowMs, mirrorTimerBars(c, m_bars, bounds), bounds);
}

} // namespace vd
