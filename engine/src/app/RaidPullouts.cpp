#include "app/RaidPullouts.h"

#include "app/AuraLayout.h"
#include "app/AuraScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/InventoryScreens.h"
#include "app/MinimapView.h"
#include "app/RaidPulloutSamples.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/CharacterNames.h"
#include "game/GameData.h"
#include "game/RaidRoster.h"
#include "game/SpellText.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kErrorColor = ui::rgba(255, 26, 26); // UIErrorsFrame (RED_FONT_COLOR)
constexpr u64 kFnvBasis = 1469598103934665603ull;

u64 mix(u64 hash, u64 value) {
    return (hash ^ value) * 1099511628211ull;
}

game::RaidAuraSpell auraSpellOf(const game::SpellInfo& info) {
    game::RaidAuraSpell s;
    s.attributes = info.attributes;
    s.attributesEx3 = info.attributesEx3;
    for (u32 i = 0; i < 3; ++i) {
        s.effects[i] = info.effects[i];
        s.effectAuras[i] = info.effectAura[i];
        s.implicitTargetA[i] = info.implicitTargetA[i];
        s.effectMisc[i] = info.effectMisc[i];
    }
    return s;
}

} // namespace

void RaidPullouts::init(const game::GameData* data, const game::AbilityData* abilities, AuraScreens* auras,
                        InventoryScreens* inventory, MessageSink sink, void* sinkContext) {
    m_data = data;
    m_abilities = abilities;
    m_auras = auras;
    m_inventory = inventory;
    m_message = sink;
    m_messageContext = sinkContext;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    // Development: VD_RAID_DEMO=1 (a few frames) or all (every group and two classes) of the sample raid,
    // offline; VD_RAID_PULLOUTS=g1,g3,c5 opens those frames in a real raid (subgroups 1..8, ChrClasses ids).
    if (const char* list = VD_DEV_ENV("VD_RAID_PULLOUTS")) {
        std::snprintf(m_autoList.data(), m_autoList.size(), "%s", list);
    }
    if (const char* demo = VD_DEV_ENV("VD_RAID_DEMO");
        demo && demo[0] != '\0' && std::strcmp(demo, "0") != 0) {
        m_demo = std::strcmp(demo, "all") == 0 ? 2 : 1;
    }
}

void RaidPullouts::setFilters(bool dispellableDebuffs, bool castableBuffs) {
    m_dispellableOnly = dispellableDebuffs;
    m_castableOnly = castableBuffs;
}

void RaidPullouts::setKeepClear(std::span<const ui::Rect> rects) {
    m_keepClearCount = 0;
    for (const ui::Rect& r : rects) {
        if (r.w > 0.0f && r.h > 0.0f && m_keepClearCount < kMaxKeepClear) {
            m_keepClear[m_keepClearCount++] = r;
        }
    }
}

void RaidPullouts::keepClearOfHud(const Hud& hud, const ui::Rect& stickPad, const ui::Rect& microMenu,
                                  const DialogContext& c) {
    const MinimapLayout map = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    const ui::Rect minimap{map.left, map.top, c.safeArea.right() - map.left,
                           178.0f * map.s}; // to MinimapZoomOut
    const ui::Rect rects[] = {stickPad,
                              hud.actionBarRect(),
                              hud.stanceBarRect(),
                              hud.extraBarRect(),
                              hud.playerFrameRect(),
                              hud.targetFrameRect(),
                              microMenu,
                              minimap};
    setKeepClear(rects);
}

u32 RaidPullouts::shownCount() const {
    u32 count = 0;
    for (u32 i = 0; i < m_frameCount; ++i) {
        count += m_frames[i].shown ? 1u : 0u;
    }
    return count;
}

void RaidPullouts::closeAll() {
    for (Frame& f : m_frames) {
        f.shown = false;
        f.dragging = false;
    }
    m_menuFrame = -1;
}

void RaidPullouts::message(u32 color, std::string_view line) {
    if (m_message && !line.empty()) {
        char text[256];
        std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(line.size()), line.data());
        m_message(m_messageContext, color, false, text);
    }
}

// --- Opening
// -------------------------------------------------------------------------------------------------------

bool RaidPullouts::openGroup(GameSession& session, u8 subgroup, f32 x, f32 y) {
    return subgroup < net::kRaidSubgroups && open(session, false, subgroup, x, y);
}

bool RaidPullouts::openClass(GameSession& session, u8 cls, f32 x, f32 y) {
    return cls != 0 && open(session, true, cls, x, y);
}

i32 RaidPullouts::frameFor(bool byClass, u8 filter) {
    // RaidPullout_GetFrame: nothing when a shown frame has that filter, else the first hidden frame, else a
    // new one.
    for (u32 i = 0; i < m_frameCount; ++i) {
        if (m_frames[i].shown && m_frames[i].byClass == byClass && m_frames[i].filter == filter) {
            return -1;
        }
    }
    for (u32 i = 0; i < m_frameCount; ++i) {
        if (!m_frames[i].shown) {
            return static_cast<i32>(i);
        }
    }
    if (m_frameCount == m_frames.size()) {
        return -1;
    }
    m_frames[m_frameCount] = Frame{};
    m_order[m_frameCount] = static_cast<u8>(m_frameCount);
    return static_cast<i32>(m_frameCount++);
}

bool RaidPullouts::open(GameSession& session, bool byClass, u8 filter, f32 x, f32 y) {
    const i32 index = frameFor(byClass, filter);
    if (index < 0) {
        return false;
    }
    Frame& f = m_frames[static_cast<u32>(index)];
    // RaidPullout_Generate*Frame: the filter, Show Buffs unchecked; a frame used again keeps its backdrop's
    // state.
    f.byClass = byClass;
    f.filter = filter;
    f.showBuffs = false;
    f.dragging = false;
    gatherRoster(session);
    listMembers(f);
    if (f.count == 0) {
        f.shown = false; // RaidPullout_Update hides a frame with nobody to list
        return false;
    }
    // RaidPulloutButton_OnDragStart: its TOP at the finger; placed on the next draw (ValidateFramePosition).
    f.unit = raidPulloutUnit(m_baseUnit, f.count, m_safe.h);
    f.x = x - kRaidPulloutWidth * f.unit * 0.5f;
    f.y = y;
    f.place = true;
    f.shown = true;
    raise(static_cast<u32>(index));
    if (m_trace) {
        VD_LOG_INFO("Raid pullout: %s %u opened with %u members", byClass ? "class" : "group",
                    byClass ? filter : filter + 1u, f.count);
    }
    return true;
}

void RaidPullouts::raise(u32 frame) {
    // Toplevel frames: the one touched goes over the others (the last drawn).
    u32 at = 0;
    while (at < m_frameCount && m_order[at] != frame) {
        ++at;
    }
    for (; at + 1 < m_frameCount; ++at) {
        m_order[at] = m_order[at + 1];
    }
    if (m_frameCount > 0) {
        m_order[m_frameCount - 1] = static_cast<u8>(frame);
    }
}

// --- The roster
// ----------------------------------------------------------------------------------------------------

u32 RaidPullouts::gatherRoster(GameSession& session) {
    VD_PROFILE_ZONE("RaidPullouts::roster");
    m_rosterCount = 0;
    if (m_demo != 0 && !session.inWorld()) {
        const std::span<const RaidSampleMember> samples = raidSampleMembers();
        for (u32 i = 0; i < samples.size() && i < m_roster.size(); ++i) {
            const RaidSampleMember& s = samples[i];
            game::PartyMemberView& v = m_roster[m_rosterCount++];
            v = game::PartyMemberView{};
            v.guid = 0x7000 + i;
            v.name = s.name;
            v.cls = s.cls;
            v.race = s.race;
            v.gender = s.gender;
            v.subgroup = s.subgroup;
            v.online = !s.offline;
            v.dead = s.dead;
            v.inSight = s.inSight && !s.offline;
            v.self = i == kRaidSampleSelf;
        }
        return m_rosterCount;
    }
    if (!session.inWorld()) {
        return 0;
    }
    m_rosterCount = session.play().social().raidMembers(m_roster);
    return m_rosterCount;
}

void RaidPullouts::listMembers(Frame& f) const {
    // RAID_SUBGROUP_LISTS[filterID] (RaidGroupFrame_Update): the members of that subgroup, or of that class
    // (by its file name: a member whose class is not known yet is in no class list), in raid index order.
    f.count = 0;
    for (u32 i = 0; i < m_rosterCount && f.count < f.members.size(); ++i) {
        const game::PartyMemberView& m = m_roster[i];
        if (f.byClass ? (m.cls != 0 && m.cls == f.filter) : m.subgroup == f.filter) {
            f.members[f.count++] = static_cast<u8>(i);
        }
    }
}

const RaidPullouts::SpellMemo* RaidPullouts::spellMemo(u32 spellId) {
    if (spellId == 0) {
        return nullptr;
    }
    if (m_memoCount >= kSpellMemoSize * 3 / 4) { // full: start over (only after hundreds of different auras)
        m_memo.fill(SpellMemo{});
        m_memoCount = 0;
    }
    u32 at = (spellId * 2654435761u) & (kSpellMemoSize - 1);
    while (m_memo[at].spellId != 0 && m_memo[at].spellId != spellId) {
        at = (at + 1) & (kSpellMemoSize - 1);
    }
    SpellMemo& e = m_memo[at];
    if (e.spellId == spellId) {
        return &e;
    }
    e = SpellMemo{};
    e.spellId = spellId;
    ++m_memoCount;
    if (m_data) {
        if (const std::optional<game::SpellInfo> info = m_data->spell(spellId)) {
            e.iconHash = info->iconHash;
            e.spell = auraSpellOf(*info);
        }
    }
    if (m_abilities) {
        game::SpellDetails details;
        if (m_abilities->spellText().details(spellId, details)) {
            e.spell.attributesEx = details.attributesEx;
            e.dispel = details.dispel;
        }
    }
    e.listed = game::raidAuraListed(e.spell);
    return &e;
}

void RaidPullouts::updateDispelTypes(GameSession& session) {
    // WoW.exe adds a spell's dispel types to the list as it is learned: the known spells' ones, when they
    // change.
    const game::SpellBook& book = session.play().spells();
    if (book.knownVersion() == m_knownVersion || !m_data) {
        return;
    }
    VD_PROFILE_ZONE("RaidPullouts::dispelTypes");
    m_knownVersion = book.knownVersion();
    m_dispelTypes = 0;
    for (const u32 spellId : book.known()) {
        if (const std::optional<game::SpellInfo> info = m_data->spell(spellId)) {
            m_dispelTypes |= game::raidDispelTypes(auraSpellOf(*info));
        }
    }
}

void RaidPullouts::memberAuras(GameSession& session, u32 index, bool buffs, RaidPulloutMemberView& out) {
    // RefreshBuffs(button, showBuffs, "raidN"): UnitBuff(unit, 1..4, SHOW_CASTABLE_BUFFS) or
    // UnitDebuff(unit, 1..4, SHOW_DISPELLABLE_DEBUFFS), the auras the client lists in slot order, those the
    // filter keeps.
    const game::PartyMemberView& m = m_roster[index];
    AuraCache& cache = m_auraCache[index][buffs ? 1 : 0];
    game::Gameplay& play = session.play();
    const u32 n = play.auras(m.guid, m_auraScratch);
    const bool filtered = buffs ? m_castableOnly : m_dispellableOnly;
    u64 signature = mix(mix(kFnvBasis, filtered ? 1 : 0), buffs ? m_knownVersion : m_dispelTypes);
    for (u32 i = 0; i < n; ++i) {
        if (m_auraScratch[i].positive() == buffs) {
            signature =
                mix(signature, static_cast<u64>(m_auraScratch[i].spellId) << 8 | m_auraScratch[i].slot);
        }
    }
    if (cache.guid != m.guid || cache.signature != signature) {
        cache.guid = m.guid;
        cache.signature = signature;
        cache.count = 0;
        for (u32 i = 0; i < n && cache.count < kRaidPulloutAuras; ++i) {
            const game::AuraInfo& a = m_auraScratch[i];
            if (a.positive() != buffs) {
                continue;
            }
            const SpellMemo* spell = spellMemo(a.spellId);
            if (!spell || !spell->listed || spell->iconHash == 0) {
                continue;
            }
            if (filtered && (buffs ? !game::raidCastableBuff(spell->spell, play.spells().knows(a.spellId))
                                   : !game::raidDispellable(m_dispelTypes, spell->dispel))) {
                continue;
            }
            // The border: the debuff's type colour, "none" red for the rest (RefreshBuffs leaves buffs
            // untyped).
            cache.views[cache.count] = {spell->iconHash, debuffTypeColor(buffs ? 0 : spell->dispel)};
            cache.spells[cache.count] = a.spellId;
            cache.slots[cache.count] = a.slot;
            ++cache.count;
        }
    }
    out.auras = cache.views;
    out.auraCount = cache.count;
}

void RaidPullouts::memberView(GameSession& session, u32 index, bool buffs, u32 nowMs,
                              RaidPulloutMemberView& out) {
    if (m_demo != 0 && !session.inWorld()) {
        out = raidSampleView(raidSampleMembers()[index], buffs, nowMs);
        return;
    }
    const game::PartyMemberView& m = m_roster[index];
    out = RaidPulloutMemberView{};
    out.name = m.name;
    if (!m.online) {
        // RaidPullout_Update's offline branch: a gray name over full gray bars (UnitFrameManaBar_Update's
        // disconnected state).
        out.nameColor = kRaidOfflineColor;
        out.health = 1.0f;
        out.healthColor = kRaidOfflineColor;
        out.power = 1.0f;
        out.powerColor = kRaidOfflineColor;
        return;
    }
    // RaidPulloutButton_UpdateDead: red while dead, else RAID_CLASS_COLORS (a class not known yet keeps the
    // font's).
    const u32 colour = game::raidClassColor(m.cls);
    out.nameColor = m.dead ? kRaidDeadColor : colour != 0 ? colour : kRaidTitleColor;
    out.health = m.maxHealth > 0 ? static_cast<f32>(m.health) / static_cast<f32>(m.maxHealth) : 0.0f;
    out.power = m.maxPower > 0 ? static_cast<f32>(m.power) / static_cast<f32>(m.maxPower) : 0.0f;
    out.powerColor = raidPowerColor(m.powerType);
    memberAuras(session, index, buffs, out);
}

// --- Every frame
// ---------------------------------------------------------------------------------------------------

std::string_view RaidPullouts::title(const Frame& f, std::span<char> out) const {
    if (f.byClass) { // the class button's text: the class's name
        const std::string_view name = game::className(f.filter);
        return name.empty() ? text::get("UNKNOWN") : name;
    }
    const std::string_view group = text::get("GROUP"); // GROUP .. " " .. groupID
    const int n = std::snprintf(out.data(), out.size(), "%.*s %u", static_cast<int>(group.size()),
                                group.data(), f.filter + 1u);
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

RaidPulloutGeometry RaidPullouts::geometry(const Frame& f) const {
    RaidPulloutGeometry g;
    g.x = f.x;
    g.y = f.y;
    g.unit = f.unit;
    g.members = f.count;
    return g;
}

void RaidPullouts::openListed(GameSession& session, const DialogContext& c, const char* list) {
    // "g3" a subgroup (1..8), "c5" a class (ChrClasses id), comma separated; side by side from the top of the
    // screen.
    const f32 unit = m_baseUnit * kRaidPulloutScale;
    const f32 cellW = (kRaidPulloutReach + 8.0f) * unit;
    const f32 cellH =
        (kRaidPulloutTitle + 5.0f * kRaidPulloutButtonHeight + kRaidPulloutPadding + 8.0f) * unit;
    const f32 left = c.safeArea.x + c.safeArea.w * 0.2f;
    const ui::Rect space{left, c.safeArea.y + c.safeArea.h * 0.18f,
                         std::max(c.safeArea.right() - left, cellW), std::max(c.safeArea.h * 0.55f, cellH)};
    u32 cell = 0;
    for (const char* p = list; *p != '\0';) {
        const char kind = *p++;
        const long n = std::strtol(p, const_cast<char**>(&p), 10);
        const ui::Rect where = raidPulloutDemoCell(cell, space, cellW, cellH);
        const f32 x = where.x + kRaidPulloutWidth * unit * 0.5f, y = where.y + kRaidPulloutTitle * unit;
        const bool opened = kind == 'g' ? n >= 1 && n <= 8 && openGroup(session, static_cast<u8>(n - 1), x, y)
                            : kind == 'c' ? n >= 1 && n < 256 && openClass(session, static_cast<u8>(n), x, y)
                                          : false;
        cell += opened ? 1u : 0u;
        while (*p == ',' || *p == ' ') {
            ++p;
        }
    }
}

u64 RaidPullouts::unitAt(f32 x, f32 y, ui::Rect& row) const {
    // Phase 20 (assists): the frames in their drawing order, the last (on top) first; the roster as last gathered.
    for (u32 o = m_frameCount; o-- > 0;) {
        const Frame& f = m_frames[m_order[o]];
        if (!f.shown || !geometry(f).hit().contains(x, y)) {
            continue;
        }
        const RaidPulloutGeometry at = geometry(f);
        for (u32 i = 0; i < f.count; ++i) {
            if (at.row(i).contains(x, y) && f.members[i] < m_rosterCount) {
                row = at.row(i);
                return m_roster[f.members[i]].guid;
            }
        }
        return 0; // its title or its auras' column
    }
    return 0;
}

void RaidPullouts::draw(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("RaidPullouts::draw");
    const bool demo = m_demo != 0 && !session.inWorld();
    m_safe = c.safeArea;
    m_baseUnit = c.ui.px(ui::kUnitDp);
    const f32 baseUnit = m_baseUnit;
    const u32 rosterCount = gatherRoster(session);
    if (rosterCount == 0) {
        if (shownCount() != 0 && m_trace) {
            VD_LOG_INFO("Raid pullout: no raid, every frame closes");
        }
        closeAll(); // RAID_ROSTER_UPDATE out of a raid: every list is empty
        m_autoOpened = false;
        m_autoRosterCount = 0;
        return;
    }
    // Development: VD_RAID_DEMO's frames, or VD_RAID_PULLOUTS' each time a raid starts, as if dragged out of
    // the raid tab (a real raid's once it kept its size for 15 s: the groups are empty while it fills).
    if (rosterCount != m_autoRosterCount) {
        m_autoRosterCount = rosterCount;
        m_autoSinceMs = nowMs;
    }
    const bool settled = demo || nowMs - m_autoSinceMs >= 15000;
    if (!m_autoOpened && settled && (demo || m_autoList[0] != '\0')) {
        m_autoOpened = true;
        openListed(session, c,
                   demo ? (m_demo == 2 ? "g1,g2,g3,g4,g5,g6,g7,g8,c5,c1" : "g1,g3,c5,g8")
                        : m_autoList.data());
        for (u32 i = 0; i < m_frameCount && demo; ++i) {
            m_frames[i].showBuffs =
                m_frames[i].byClass && m_frames[i].filter == 5; // the priests with their buffs
        }
    }
    if (!demo) {
        updateDispelTypes(session);
    }

    i32 touched = -1;
    u32 demoPlaced = 0;
    for (u32 o = 0; o < m_frameCount; ++o) {
        const u32 index = m_order[o];
        Frame& f = m_frames[index];
        if (!f.shown) {
            continue;
        }
        listMembers(f); // RAID_ROSTER_UPDATE: joined, left, moved, another class known
        if (f.count == 0) {
            f.shown = false; // nobody to list: the frame hides
            f.dragging = false;
            continue;
        }
        f.unit = raidPulloutUnit(baseUnit, f.count, c.safeArea.h);
        // ValidateFramePosition after it opened or was dropped (and off the HUD's controls); clampedToScreen
        // always.
        // The demo's frames, opened together, also keep off each other while there is room.
        RaidPulloutGeometry at = geometry(f);
        const ui::Rect hit = at.hit();
        const u32 avoid = demo && f.place ? m_keepClearCount + demoPlaced : m_keepClearCount;
        const ui::Rect placed = raidPulloutPlace(
            hit, c.safeArea,
            f.place ? std::span<const ui::Rect>(m_keepClear.data(), avoid) : std::span<const ui::Rect>{});
        if (m_trace && f.place) {
            VD_LOG_INFO("Raid pullout: placed at (%.0f, %.0f) -> (%.0f, %.0f), clear of %u rectangles", hit.x,
                        hit.y, placed.x, placed.y, avoid);
        }
        if (demo && f.place && m_keepClearCount + demoPlaced < m_keepClear.size()) {
            m_keepClear[m_keepClearCount + demoPlaced++] = placed;
        }
        f.place = false;
        f.x += placed.x - hit.x;
        f.y += placed.y - hit.y;
        at = geometry(f);

        for (u32 j = 0; j < f.count; ++j) {
            memberView(session, f.members[j], f.showBuffs, nowMs, m_views[j]);
        }
        std::array<char, 64> titleText{};
        RaidPulloutView v;
        v.title = title(f, titleText);
        v.members = {m_views.data(), f.count};
        v.at = at;
        v.background = f.background;
        const RaidPulloutResult r = raidPullout(c, index, v);
        if (r.moving || r.held || r.tapped >= 0 || r.aura >= 0) {
            touched = static_cast<i32>(index);
        }
        act(session, f, index, r);
    }
    if (touched >= 0) {
        raise(static_cast<u32>(touched));
    }
}

void RaidPullouts::act(GameSession& session, Frame& f, u32 frameIndex, const RaidPulloutResult& r) {
    const bool demo = m_demo != 0 && !session.inWorld();
    // OnDragStart / OnDragStop: the frame follows the finger, then ValidateFramePosition.
    if (r.moving) {
        if (!f.dragging) {
            f.dragging = true;
            f.dragX = f.x;
            f.dragY = f.y;
        }
        f.x = f.dragX + r.fingerDx;
        f.y = f.dragY + r.fingerDy;
        if (r.dropped) {
            f.dragging = false;
            f.place = true;
        }
        return;
    }
    if (r.held) {
        m_menuFrame = static_cast<i32>(frameIndex); // the right click: RaidPulloutDropDown
        return;
    }
    if (r.aura >= 0 && r.auraMember >= 0 && static_cast<u32>(r.auraMember) < f.count) {
        const u32 index = f.members[static_cast<u32>(r.auraMember)];
        const AuraCache& cache = m_auraCache[index][f.showBuffs ? 1 : 0];
        if (!demo && m_auras && static_cast<u32>(r.aura) < cache.count) {
            m_auras->showAuraTooltip(m_roster[index].guid, cache.spells[static_cast<u32>(r.aura)],
                                     cache.slots[static_cast<u32>(r.aura)], r.auraRect);
        } else if (demo) {
            VD_LOG_INFO("Raid pullout demo: aura %d of %.*s", r.aura,
                        static_cast<int>(m_roster[index].name.size()), m_roster[index].name.data());
        }
        return;
    }
    if (r.tapped >= 0 && static_cast<u32>(r.tapped) < f.count) {
        // RaidPulloutButton_OnClick: TargetUnit (Gameplay::setTarget; no spell waits for a unit here). The client
        // can only select a unit it sees.
        const game::PartyMemberView& m = m_roster[f.members[static_cast<u32>(r.tapped)]];
        if (demo) {
            VD_LOG_INFO("Raid pullout demo: target %.*s", static_cast<int>(m.name.size()), m.name.data());
        } else if (m.inSight) {
            session.setTarget(m.guid);
        }
        if (m_trace && !demo) {
            VD_LOG_INFO("Raid pullout: tap on %.*s (%s)", static_cast<int>(m.name.size()), m.name.data(),
                        m.inSight ? "targeted" : "out of sight");
        }
    }
}

void RaidPullouts::drawOverlay(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("RaidPullouts::overlay");
    (void)nowMs;
    // An item carried and let go on a member (RaidPulloutButton_OnClick: DropItemOnUnit), the topmost frame
    // first.
    f32 dropX = 0.0f, dropY = 0.0f;
    if (m_inventory && session.inWorld() && m_rosterCount > 0 &&
        m_inventory->releasedItemAt(c, dropX, dropY)) {
        for (u32 o = m_frameCount; o-- > 0;) {
            const Frame& f = m_frames[m_order[o]];
            if (!f.shown) {
                continue;
            }
            const RaidPulloutGeometry at = geometry(f);
            for (u32 j = 0; j < f.count; ++j) {
                if (at.row(j).contains(dropX, dropY)) {
                    message(kErrorColor, m_inventory->dropOnUnit(session, m_roster[f.members[j]].guid));
                    o = 0;
                    break;
                }
            }
        }
    }
    // RaidPulloutDropDown_Initialize: Show Buffs and Show Debuffs (exclusive), Hide Background, Close.
    if (m_menuFrame < 0) {
        return;
    }
    Frame& f = m_frames[static_cast<u32>(m_menuFrame)];
    if (!f.shown) {
        m_menuFrame = -1;
        return;
    }
    m_menuItems[0] = {text::get("SHOW_BUFFS"), 0, false, true, true, f.showBuffs};
    m_menuItems[1] = {text::get("SHOW_DEBUFFS"), 0, false, true, true, !f.showBuffs};
    m_menuItems[2] = {text::get("HIDE_PULLOUT_BG"), 0, false, true, true, !f.background};
    m_menuItems[3] = {text::get("CLOSE"), 0, false, true, true, false};
    DropDownMenuView v;
    const ui::Rect frame = geometry(f).frame();
    v.x = frame.right(); // UIDropDownMenu_SetAnchor(0, 0, this, "TOPLEFT", <pullout>, "TOPRIGHT")
    v.y = frame.y;
    v.level1 = m_menuItems;
    const DropDownMenuResult r = dropDownMenu(c, "raid.pullout.menu", v);
    if (r.level == 1 && r.picked >= 0) {
        switch (r.picked) {
        case 0: f.showBuffs = true; break;
        case 1: f.showBuffs = false; break;
        case 2: f.background = !f.background; break;
        default: f.shown = false; break; // Close: currentPullout:Hide()
        }
        if (m_trace) {
            VD_LOG_INFO("Raid pullout: menu choice %d", r.picked);
        }
        m_menuFrame = -1;
    } else if (r.closed) {
        m_menuFrame = -1;
    }
}

} // namespace vd
