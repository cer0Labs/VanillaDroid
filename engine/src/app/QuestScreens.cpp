#include "app/QuestScreens.h"

#include "app/ChatLinkInsert.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/Chat.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/Travel.h"
#include "net/QuestMessages.h"
#include "net/UpdateFields.gen.h"
#include "ui/DrawList.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>

namespace vd {
namespace {

namespace uf = net::uf;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26); // UIErrorsFrame.lua: UI_ERROR_MESSAGE
constexpr u32 kInfoColor = rgba(255, 255, 0);  // UI_INFO_MESSAGE
constexpr usize kMaxObjectiveViews = game::kQuestLogSize * 8 + game::kMaxWatchedQuests * 8;

// The client's line of a MSG_QUEST_PUSH_RESULT (WoW.exe: one ERR_QUEST_PUSH_*_S per QuestShareMessages
// value).
const char* pushResultKey(u8 result) {
    switch (result) {
    case net::kQuestPushSharing: return "ERR_QUEST_PUSH_SUCCESS_S";
    case net::kQuestPushCantTake: return "ERR_QUEST_PUSH_INVALID_S";
    case net::kQuestPushAccepted: return "ERR_QUEST_PUSH_ACCEPTED_S";
    case net::kQuestPushDeclined: return "ERR_QUEST_PUSH_DECLINED_S";
    case net::kQuestPushTooFar: return "ERR_QUEST_PUSH_TOO_FAR_S";
    case net::kQuestPushBusy: return "ERR_QUEST_PUSH_BUSY_S";
    case net::kQuestPushLogFull: return "ERR_QUEST_PUSH_LOG_FULL_S";
    case net::kQuestPushOnQuest: return "ERR_QUEST_PUSH_ONQUEST_S";
    case net::kQuestPushAlreadyDone: return "ERR_QUEST_PUSH_ALREADY_DONE_S";
    default: return nullptr;
    }
}

} // namespace

QuestScreens::QuestScreens() {
    m_rows.reserve(game::QuestJournal::kMaxRows);
    m_objectiveViews.reserve(kMaxObjectiveViews);
    m_choices.reserve(6);
    m_rewards.reserve(4);
    m_watchViews.reserve(game::kMaxWatchedQuests);
    m_watchQuests.reserve(game::kMaxWatchedQuests);
    m_timerTexts.resize(game::kQuestLogSize);
    for (std::string& s : m_timerTexts) {
        s.reserve(64);
    }
    m_timerViews.reserve(game::kQuestLogSize);
    m_timerQuests.reserve(game::kQuestLogSize);
    for (std::string& s : m_expanded) {
        s.reserve(2048);
    }
}

std::string_view QuestScreens::expand(const Frame& f, std::string_view text, u32 buffer) {
    game::TextSubject subject;
    if (const game::Entity* me = f.entities.find(f.player)) {
        subject.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
        subject.cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        subject.gender = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    }
    if (const std::string* name = f.play.queries().findPlayerName(f.player)) {
        subject.name = *name;
    }
    std::string& out = m_expanded[buffer % m_expanded.size()];
    game::expandText(text, subject, out);
    return out;
}

void QuestScreens::init(const pack::PackReader& db, const game::GameData* data, const game::AreaData* areas) {
    m_data.init(db, areas);
    m_gameData = data;
}

ItemView QuestScreens::itemView(game::Gameplay& play, u32 entry, u32 count) {
    ItemView view;
    view.count = count;
    view.entry = entry;
    if (const net::ItemQueryInfo* info = play.queries().item(entry)) {
        view.name = info->name;
        view.quality = info->quality;
        view.usable = play.inventory().usable(*info); // GetQuestLogChoiceInfo / GetQuestLogRewardInfo's isUsable
        view.iconHash = m_gameData           ? m_gameData->itemIconHash(info->displayId)
                        : m_sources.itemIcon ? m_sources.itemIcon(m_sources.context, entry)
                                             : 0;
    } else {
        view.name = "...";
    }
    return view;
}

QuestSpellRewardView QuestScreens::spellView(u32 spellId) const {
    QuestSpellRewardView view;
    if (m_gameData) {
        if (const std::optional<game::SpellInfo> spell = m_gameData->spell(spellId)) {
            view.iconHash = spell->iconHash;
            view.name = spell->name;
            // GetQuestLogRewardSpell's third value: a spell that makes something (SPELL_EFFECT_CREATE_ITEM).
            constexpr u32 kEffectCreateItem = 24;
            view.tradeskill = std::find(std::begin(spell->effects), std::end(spell->effects),
                                        kEffectCreateItem) != std::end(spell->effects);
        }
    } else if (m_sources.spell) {
        view = m_sources.spell(m_sources.context, spellId);
    }
    return view;
}

void QuestScreens::restart(const game::Gameplay& play) {
    m_eventCursor = play.events().end();
}

void QuestScreens::line(u32 color, const char* text) {
    if (m_sink && text[0] != '\0') {
        m_sink(m_sinkContext, color, false, text);
    }
}

std::string_view QuestScreens::partyName(const Frame& f, u64 guid) const {
    for (const game::GroupMember& m : f.play.social().group().members) {
        if (m.guid == guid && !m.name.empty()) {
            return m.name;
        }
    }
    const std::string* name = f.play.queries().findPlayerName(guid);
    return name ? std::string_view(*name) : std::string_view{};
}

void QuestScreens::readEvents(const Frame& f) {
    using K = game::GameEventKind;
    char buffer[256];
    while (const game::GameEvent* e = f.play.events().next(m_eventCursor)) {
        switch (e->kind) {
        case K::QuestPushResult: {
            // How sharing went with one member (QuestLogPushQuest's answers), in the middle of the screen.
            if (const char* key = pushResultKey(static_cast<u8>(e->value[0]))) {
                const std::string_view name = partyName(f, e->guid);
                text::format(buffer, key, name.empty() ? std::string_view("...") : name);
                line(kInfoColor, buffer);
            }
            break;
        }
        case K::FishNotHooked: line(kErrorColor, text::format(buffer, "ERR_FISH_NOT_HOOKED").data()); break;
        case K::FishEscaped: line(kErrorColor, text::format(buffer, "ERR_FISH_ESCAPED").data()); break;
        default: break;
        }
    }
}

std::span<const ObjectiveView> QuestScreens::objectives(game::Gameplay& play, u32 questId) {
    const usize first = m_objectiveViews.size();
    const u32 n = play.questObjectives(questId, m_objectiveScratch);
    for (u32 i = 0; i < n && m_objectiveViews.size() < kMaxObjectiveViews; ++i) {
        const game::QuestObjectiveView& o = m_objectiveScratch[i];
        // GetQuestLogLeaderBoard: the quest's own text for it, or the name: QUEST_MONSTERS_KILLED
        // ("<creature> slain: 3/10") for kills, "<name>: n/m" for objects and items.
        const bool own = !o.text.empty();
        m_objectiveViews.push_back({own              ? o.text
                                    : o.name.empty() ? std::string_view("...")
                                                     : o.name,
                                    o.have, o.need,
                                    !own && o.kind == game::QuestObjectiveView::Kind::Creature});
    }
    return {m_objectiveViews.data() + first, m_objectiveViews.size() - first};
}

u32 QuestScreens::groupMates(const Frame& f, u32 questId) const {
    // IsUnitOnQuest of each party member in sight ("party1".."partyN": phase 16, a raid's own subgroup): their quest
    // log ids are group-only update fields.
    u32 mates = 0;
    for (const game::GroupMember& m : f.play.social().group().members) {
        const game::Entity* e = f.entities.find(m.guid);
        if (!e || e->type != game::ObjectType::Player || !f.play.social().inPartyWith(m.guid)) {
            continue;
        }
        for (u32 slot = 0; slot < game::kQuestLogSize; ++slot) {
            if (e->value(static_cast<u16>(uf::PLAYER_QUEST_LOG_1_1 + slot * 3)) == questId) {
                ++mates;
                break;
            }
        }
    }
    return mates;
}

void QuestScreens::buildViews(const Frame& f) {
    game::Gameplay& play = f.play;
    const game::QuestLog& log = play.questLog();
    const game::QuestJournal& journal = play.questJournal();
    m_rows.clear();
    m_objectiveViews.clear();
    m_choices.clear();
    m_rewards.clear();
    m_hasDetail = false;
    m_selectedRow = -1;

    // The list (GetQuestLogTitle): headers with their names, quests with their tags and marks.
    for (const game::QuestLogRow& row : journal.rows()) {
        QuestLogRowView v;
        v.header = row.header;
        v.collapsed = row.collapsed;
        if (row.header) {
            v.title = m_data.headerName(row.zoneOrSort);
        } else {
            const game::QuestInfo* info = play.queries().findQuest(row.questId);
            const game::QuestLogEntry* entry = log.find(row.questId);
            v.title = info ? std::string_view(info->title) : std::string_view("...");
            v.level = info ? info->level : 0;
            v.tag = info ? m_data.typeName(info->type) : std::string_view{};
            v.complete = entry && entry->complete();
            v.failed = entry && entry->failed();
            v.watched = journal.watched(row.questId);
            v.groupMates = groupMates(f, row.questId);
            if (row.questId == journal.selected()) {
                m_selectedRow = static_cast<i32>(m_rows.size());
            }
        }
        m_rows.push_back(v);
    }

    // The selected quest (QuestLog_UpdateQuestDetails).
    const u32 selected = journal.selected();
    const game::QuestInfo* info = selected != 0 ? play.quest(selected) : nullptr;
    const game::QuestLogEntry* entry = selected != 0 ? log.find(selected) : nullptr;
    if (entry) {
        QuestLogDetailView& d = m_detail;
        d = QuestLogDetailView{};
        d.title = info ? std::string_view(info->title) : std::string_view("...");
        d.failed = entry->failed();
        if (const u32 left = game::questTimeLeft(*entry, static_cast<u32>(f.serverNow));
            left > 0 && !entry->failed()) {
            d.timeLeft = game::secondsToTime(m_timeLeft, left);
        }
        d.objectives = objectives(play, selected);
        if (info) {
            d.objectivesText = expand(f, info->objectives, 0);
            d.description = expand(f, info->details, 1);
            // RewOrReqMoney: negative is what the quest takes (GetQuestLogRequiredMoney), positive what it
            // gives.
            if (info->money < 0) {
                d.requiredMoney = static_cast<u32>(-info->money);
                const game::Entity* me = f.entities.find(f.player);
                d.notEnoughMoney = me && me->value(uf::PLAYER_FIELD_COINAGE) < d.requiredMoney;
            } else {
                d.money = static_cast<u32>(info->money);
            }
            for (const net::QuestItem& item : info->choices) {
                if (item.itemId != 0) {
                    m_choices.push_back(itemView(play, item.itemId, item.count));
                }
            }
            for (const net::QuestItem& item : info->rewards) {
                if (item.itemId != 0) {
                    m_rewards.push_back(itemView(play, item.itemId, item.count));
                }
            }
            if (info->rewardSpell != 0) {
                d.spell = spellView(info->rewardSpell);
            }
        }
        d.choices = m_choices;
        d.rewards = m_rewards;
        m_hasDetail = true;
    }

    // QuestWatch_Update: the watched quests with objectives, in the order they were watched.
    m_watchViews.clear();
    m_watchQuests.clear();
    for (const game::QuestWatch& w : journal.watches()) {
        const game::QuestInfo* q = play.queries().findQuest(w.questId);
        if (!q) {
            continue;
        }
        const std::span<const ObjectiveView> o = objectives(play, w.questId);
        if (o.empty()) {
            continue; // a quest without objectives shows nothing
        }
        m_watchViews.push_back({q->title, o});
        m_watchQuests.push_back(w.questId);
    }

    // GetQuestTimers: the timed quests running, in log order.
    m_timerViews.clear();
    m_timerQuests.clear();
    for (const game::QuestLogEntry& slot : log.slots()) {
        const u32 left = slot.questId != 0 && !slot.failed()
                             ? game::questTimeLeft(slot, static_cast<u32>(f.serverNow))
                             : 0;
        if (left == 0) {
            continue;
        }
        std::string& shown = m_timerTexts[m_timerViews.size()];
        char buffer[64];
        shown.assign(game::secondsToTime(buffer, left));
        m_timerViews.push_back(shown);
        m_timerQuests.push_back(slot.questId);
    }
}

void QuestScreens::update(const Frame& f) {
    VD_PROFILE_ZONE("QuestScreens::update");
    game::QuestJournal& journal = f.play.questJournal();
    journal.setAutoWatch(f.autoWatch);
    journal.refresh(f.play.questLog(), f.play.queries(), &m_data);
    readEvents(f);
    buildViews(f);
}

void QuestScreens::select(game::Gameplay& play, u32 questId) {
    game::QuestJournal& journal = play.questJournal();
    if (const game::QuestInfo* info = play.queries().findQuest(questId)) {
        journal.expand(info->zoneOrSort); // its header unfolds so the quest stays selected
    }
    journal.select(questId);
    journal.refresh(play.questLog(), play.queries(), &m_data);
}

bool QuestScreens::drawLog(const Frame& f, DialogContext& c, u32& abandonOut, bool& scrolledOut) {
    game::Gameplay& play = f.play;
    game::QuestJournal& journal = play.questJournal();
    QuestLogPanelView v;
    v.rows = m_rows;
    v.selected = m_selectedRow;
    v.quests = 0;
    for (const game::QuestLogEntry& slot : play.questLog().slots()) {
        v.quests += slot.questId != 0 ? 1 : 0;
    }
    v.allCollapsed = journal.headerCount() > 0 && journal.allCollapsed();
    v.anyWatched = !journal.watches().empty();
    v.canShare = journal.selected() != 0 && play.canShareQuest(journal.selected());
    v.detail = m_hasDetail ? &m_detail : nullptr;
    const QuestLogPanelResult r = questLogPanel(c, v);

    const std::span<const game::QuestLogRow> rows = journal.rows();
    if (r.row >= 0 && static_cast<usize>(r.row) < rows.size()) {
        // QuestLog_SetSelection: a header folds or unfolds, a quest is selected.
        const game::QuestLogRow& row = rows[static_cast<usize>(r.row)];
        if (row.header) {
            row.collapsed ? journal.expand(row.zoneOrSort) : journal.collapse(row.zoneOrSort);
        } else {
            journal.select(row.questId);
        }
    }
    if (r.linkRow >= 0 && static_cast<usize>(r.linkRow) < m_rows.size() && c.chatLink) {
        // With the chat's edit box open the title goes into it (1.12 has no quest links: the plain title).
        c.chatLink->insert(m_rows[static_cast<usize>(r.linkRow)].title);
    }
    if (r.toggleAll) {
        // QuestLogCollapseAllButton_OnClick: all folded unfold, else everything folds.
        journal.allCollapsed() ? journal.expand(0) : journal.collapse(0);
    }
    if (r.track && journal.selected() != 0) {
        // QuestLogTitleButton_OnClick with Shift: the watch toggles, or why it cannot.
        const u32 questId = journal.selected();
        const u32 count = play.questObjectives(questId, m_objectiveScratch);
        char buffer[160];
        switch (journal.toggleWatch(questId, count)) {
        case game::QuestWatchToggle::NoObjectives:
            line(kErrorColor, text::format(buffer, "QUEST_WATCH_NO_OBJECTIVES").data());
            break;
        case game::QuestWatchToggle::TooMany:
            line(kErrorColor, text::format(buffer, "QUEST_WATCH_TOO_MANY", game::kMaxWatchedQuests).data());
            break;
        default: break;
        }
    }
    if (r.share && journal.selected() != 0) {
        play.shareQuest(journal.selected()); // QuestLogPushQuest
    }
    abandonOut = r.abandon ? journal.selected() : 0;
    scrolledOut = r.scrolled;
    if (r.row >= 0 || r.toggleAll || r.track) {
        journal.refresh(play.questLog(), play.queries(), &m_data);
    }
    return r.closed;
}

u32 QuestScreens::drawTimers(const Frame& f, DialogContext& c, f32 right, f32 top) {
    const QuestFrameResult timers = questTimerFrame(c, right, top, m_timerViews);
    const u32 tappedQuest = timers.tapped >= 0 ? m_timerQuests[static_cast<usize>(timers.tapped)] : 0;
    if (tappedQuest != 0) {
        select(f.play, tappedQuest);
    }
    return tappedQuest;
}

u32 QuestScreens::drawWatch(const Frame& f, DialogContext& c, f32 left, f32 top, f32 bottom, f32 maxWidth,
                            ui::Rect& bounds) {
    const QuestFrameResult watch = questWatchFrame(c, left, top, bottom, maxWidth, m_watchViews);
    bounds = watch.bounds;
    const u32 tappedQuest = watch.tapped >= 0 ? m_watchQuests[static_cast<usize>(watch.tapped)] : 0;
    if (tappedQuest != 0) {
        select(f.play, tappedQuest);
    }
    return tappedQuest;
}

} // namespace vd
