#include "app/Hud.h"

#include "app/ChatBubbles.h"
#include "app/FlyCamera.h"
#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/QuestMarks.h"
#include "app/UiResources.h"
#include "app/UnitMenuWindows.h"
#include "app/WorldLabels.h"
#include "app/WorldTap.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/UnitInfo.h"
#include "game/UnitText.h"
#include "net/UpdateFields.gen.h"
#include "pack/TerrainFormat.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd {

using namespace hud;

namespace {

// Phase 20 (smart, §6.5): the Target button's fan.
constexpr u64 kIconTargetEnemy = assetPathHash("Interface\\Icons\\Ability_Hunter_SniperShot.blp");
constexpr u64 kIconTargetParty = assetPathHash("Interface\\Icons\\Spell_Holy_PrayerOfHealing02.blp");
constexpr u64 kIconTargetAssist = assetPathHash("Interface\\Icons\\Ability_Hunter_EagleEye.blp");
constexpr u64 kIconTargetPet = assetPathHash("Interface\\Icons\\Ability_Hunter_BeastCall.blp");
constexpr u64 kIconTargetSelf = assetPathHash("Interface\\Icons\\INV_Misc_Head_Human_01.blp");

} // namespace

void Hud::worldControls(const HudFrame& frame, PlayerController::TouchInput& input) {
    const f32 dp = m_ui.dp();
    // In free-camera mode (development) the fly camera gets the fingers as they come.
    if (frame.freeCamera) {
        for (const ui::Touch& t : m_ui.touches()) {
            if (t.owner != 0) {
                continue;
            }
            if (t.began) {
                frame.freeCamera->onTouch(TouchPhase::Down, t.id, t.startX, t.startY, frame.width);
            }
            frame.freeCamera->onTouch(t.released ? TouchPhase::Up : TouchPhase::Move, t.id, t.x, t.y, frame.width);
        }
        m_stick = {};
        return;
    }
    // The stick: the first finger that lands on the left side (below the unit frames) until it lifts.
    const ui::Touch* stick = nullptr;
    for (const ui::Touch& t : m_ui.touches()) {
        if (t.owner == 0 && t.id == m_stick.touch) {
            stick = &t;
        }
    }
    if (!stick) {
        m_stick = {};
        for (const ui::Touch& t : m_ui.touches()) {
            // Phase 20: the left hand's layout has the cluster on the left and the stick on the right.
            const bool leftHanded = frame.touch && frame.touch->mirrored;
            const bool side = leftHanded ? t.startX > frame.width * 0.6f : t.startX < frame.width * 0.4f;
            if (t.owner == 0 && t.began && side && t.startY > frame.height * 0.3f) {
                m_stick.touch = t.id;
                m_stick.originX = t.startX;
                m_stick.originY = t.startY;
                stick = &t;
                break;
            }
        }
    }
    if (stick) {
        if (stick->released) {
            m_stick = {};
        } else {
            glm::vec2 offset((stick->x - m_stick.originX) / (kStickRadiusDp * dp),
                             -(stick->y - m_stick.originY) / (kStickRadiusDp * dp));
            const f32 length = glm::length(offset);
            if (length > 1.0f) {
                // The base follows a finger that goes beyond it, so turning back is immediate.
                const glm::vec2 excess = offset * ((length - 1.0f) / length);
                m_stick.originX += excess.x * kStickRadiusDp * dp;
                m_stick.originY -= excess.y * kStickRadiusDp * dp;
                offset /= length;
            }
            m_stick.value = offset;
            input.stick = offset;
        }
    }
    // A quick tap on the world selects what is under it; on empty ground it keeps the target (the client only
    // clears it with Escape: here a long press on the target frame). Parity: a tap on the unit already
    // selected acts on it like the client's right click, and a tap on no unit uses the game object under it
    // (app/WorldTap.h).
    for (const ui::Touch& t : m_ui.touches()) {
        const bool tap = t.owner == 0 && t.released && !t.cancelled && !t.moved && m_ui.nowMs() - t.downMs < 350 &&
                         !frame.chatRect.contains(t.x, t.y) && !frame.questWatchRect.contains(t.x, t.y);
        if (tap && frame.groundTargeting) { // phase 13 (spells): the waiting spell's circle goes there
            m_output.groundTapped = true;
            m_output.groundTapX = t.x;
            m_output.groundTapY = t.y;
            continue;
        }
        if (tap && frame.session && frame.session->inWorld()) {
            const u64 picked = pickUnit(frame, t.x, t.y);
            switch (worldTap(picked, frame.session->target())) {
            case WorldTap::Select:
                frame.session->setTarget(picked);
                m_output.unitSelected = picked;
                break;
            case WorldTap::Act: m_output.unitTapped = picked; break;
            default:
                m_output.objectTapped = pickObject(frame, t.x, t.y); // units first, then objects
                // Phase 15 (options): GAMEFIELD_DESELECT unchecked (deselectOnClick, 1.12's default): a click on the
                // world that hits nothing clears the target.
                if (m_output.objectTapped == 0 && frame.deselectOnClick && frame.session->target() != 0) {
                    frame.session->setTarget(0);
                }
                break;
            }
        }
    }
    // The camera: every other finger on the world. Two of them pinch (zoom).
    const ui::Touch* camera[2] = {nullptr, nullptr};
    u32 cameraCount = 0;
    for (const ui::Touch& t : m_ui.touches()) {
        if (t.owner == 0 && t.id != m_stick.touch && !t.released && cameraCount < 2) {
            camera[cameraCount++] = &t;
        }
    }
    if (cameraCount == 1) {
        input.yawDelta -= camera[0]->dx / dp * kCameraRadiansPerDp * frame.cameraSpeed;
        input.pitchDelta -= camera[0]->dy / dp * kCameraRadiansPerDp * frame.cameraSpeed *
                            (frame.invertCamera ? -1.0f : 1.0f);
        m_pinchDistance = 0.0f;
    } else if (cameraCount == 2) {
        const f32 distance = std::hypot(camera[0]->x - camera[1]->x, camera[0]->y - camera[1]->y) / dp;
        if (m_pinchDistance > 0.0f) {
            input.zoomSteps += (distance - m_pinchDistance) * kPinchStepsPerDp;
        }
        m_pinchDistance = distance;
    } else {
        m_pinchDistance = 0.0f;
    }
}

bool Hud::project(const HudFrame& frame, const glm::dvec3& world, f32& x, f32& y) const {
    const glm::vec4 clip = frame.viewProj * glm::vec4(glm::vec3(world - frame.camera), 1.0f);
    if (clip.w < 0.1f) {
        return false;
    }
    x = (clip.x / clip.w * 0.5f + 0.5f) * frame.width;
    y = (clip.y / clip.w * 0.5f + 0.5f) * frame.height;
    return true;
}

u64 Hud::pickUnit(const HudFrame& frame, f32 x, f32 y) const {
    if (!frame.session) {
        return 0;
    }
    const u64 self = frame.session->playerGuid();
    u64 best = 0;
    f64 bestDistance = 1e30;
    const f32 slop = m_ui.px(10.0f);
    for (const Units::UnitView& view : frame.units) {
        if (view.guid == self) {
            continue;
        }
        f32 fx, fy, hx, hy;
        if (!project(frame, view.feet, fx, fy) || !project(frame, view.head, hx, hy)) {
            continue;
        }
        // A box around the body: as tall as it looks, as wide as its girth, never smaller than a finger.
        const f32 tall = std::max(1.0f, fy - hy);
        const f32 halfWidth =
            std::max(m_ui.px(16.0f), tall * view.radius / static_cast<f32>(std::max(0.5, view.head.z - view.feet.z)));
        const f32 cx = (fx + hx) * 0.5f;
        const Rect box{cx - halfWidth - slop, hy - slop, 2.0f * (halfWidth + slop), tall + 2.0f * slop};
        if (!box.contains(x, y)) {
            continue;
        }
        const f64 distance = glm::length(view.feet - frame.camera);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = view.guid;
        }
    }
    return best;
}

u64 Hud::pickObject(const HudFrame& frame, f32 x, f32 y) const {
    if (!frame.session) {
        return 0;
    }
    u64 best = 0;
    f64 bestDistance = 1e30;
    const f32 slop = m_ui.px(10.0f);
    for (const WorldObjectView& view : frame.objects) {
        const f64 distance = glm::length(view.center - frame.camera);
        const game::Entity* e = distance < bestDistance ? frame.session->entities().find(view.guid) : nullptr;
        if (!e || !worldObjectTappable(*e)) {
            continue;
        }
        // The screen box of its bounding sphere (the ends of its three axes), never smaller than a finger.
        f32 left = 1e30f, right = -1e30f, top = 1e30f, bottom = -1e30f;
        bool shown = true;
        for (u32 i = 0; i < 6 && shown; ++i) {
            glm::dvec3 p = view.center;
            p[i / 2] += (i % 2 ? -1.0 : 1.0) * view.radius;
            f32 px, py;
            shown = project(frame, p, px, py);
            left = std::min(left, px);
            right = std::max(right, px);
            top = std::min(top, py);
            bottom = std::max(bottom, py);
        }
        if (!shown) {
            continue;
        }
        const f32 grow = std::max(0.0f, m_ui.px(32.0f) - (right - left)) * 0.5f + slop;
        const f32 growY = std::max(0.0f, m_ui.px(32.0f) - (bottom - top)) * 0.5f + slop;
        const Rect box{left - grow, top - growY, right - left + 2.0f * grow, bottom - top + 2.0f * growY};
        if (box.contains(x, y)) {
            bestDistance = distance;
            best = view.guid;
        }
    }
    return best;
}

u32 Hud::enemyCandidates(const HudFrame& frame, std::array<EnemyCandidate, 64>& out) const {
    if (!frame.session || !frame.reactions) {
        return 0;
    }
    const game::Entity* me = frame.session->entities().find(frame.session->playerGuid());
    if (!me) {
        return 0;
    }
    // Phase 20 (smart, §6.5): who fights the player or its group (its target is one of them, the unit in combat).
    const u64 pet = game::guidValue(*me, uf::UNIT_FIELD_SUMMON);
    const game::GroupState& group = frame.session->play().social().group();
    const auto attacksGroup = [&](const game::Entity& unit) {
        const u64 victim = game::inCombat(unit) ? game::guidValue(unit, uf::UNIT_FIELD_TARGET) : 0;
        if (victim == 0) {
            return false;
        }
        if (victim == me->guid || victim == pet) {
            return true;
        }
        for (const game::GroupMember& m : group.members) {
            if (m.guid == victim) {
                return true;
            }
        }
        return false;
    };
    const auto enemy = [&](const game::Entity& unit) {
        return &unit != me && game::isUnit(unit) && unit.value(uf::UNIT_FIELD_HEALTH) != 0 &&
               game::reactionTo(*me, unit, frame.reactions) != game::Reaction::Friendly &&
               !(frame.breakableControl && frame.breakableControl(frame.breakableContext, unit.guid));
    };
    // Fixed capacity (no allocation per frame).
    u32 count = 0;
    for (const Units::UnitView& view : frame.units) {
        const game::Entity* unit = frame.session->entities().find(view.guid);
        if (!unit || !enemy(*unit) || count == out.size()) {
            continue;
        }
        const f64 distance = glm::length(view.feet - frame.position);
        f32 x, y;
        if (distance > 40.0 || !project(frame, view.feet, x, y) || x < 0.0f || x > frame.width || y < 0.0f ||
            y > frame.height) {
            continue;
        }
        out[count++] = {view.guid, distance, static_cast<u8>(attacksGroup(*unit) ? 0 : 1)};
    }
    // Those fighting the group out of sight (behind the camera, not drawn): after the rest.
    const u32 onScreen = count;
    const u32 nowMs = static_cast<u32>(frame.nowMs);
    frame.session->entities().forEach([&](const game::Entity& unit) {
        if (count == out.size() || !enemy(unit) || !attacksGroup(unit)) {
            return;
        }
        for (u32 i = 0; i < onScreen; ++i) {
            if (out[i].guid == unit.guid) {
                return;
            }
        }
        const f64 distance = glm::length(glm::dvec3(unit.positionAt(nowMs)) - frame.position);
        if (distance <= 40.0) {
            out[count++] = {unit.guid, distance, 2};
        }
    });
    return count;
}

u64 Hud::nextEnemy(const HudFrame& frame, i32 step) const {
    std::array<EnemyCandidate, 64> candidates;
    const u32 count = enemyCandidates(frame, candidates);
    const u64 current = frame.session ? frame.session->target() : 0;
    if (count == 0) {
        return current;
    }
    // Phase 20 (smart, §6.5): on screen first, then those attacking the group, then the nearest.
    std::sort(candidates.begin(), candidates.begin() + count, [](const EnemyCandidate& a, const EnemyCandidate& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.distance < b.distance;
    });
    for (u32 i = 0; i < count; ++i) {
        if (candidates[i].guid == current) {
            return candidates[(i + count + (step < 0 ? count - 1u : 1u)) % count].guid;
        }
    }
    return candidates[0].guid;
}

u32 Hud::targetOptions(const HudFrame& frame, std::array<TargetOption, 5>& out) const {
    // Phase 20 (smart, §6.5): the Target button's fan.
    u32 count = 0;
    if (!frame.session || !frame.session->inWorld()) {
        if (frame.targetFanDemo) { // VD_TOUCH_SMART_DEMO: what it offers, without units
            out[count++] = {0, kIconTargetEnemy, text::get("VD_TOUCH_SMART_TARGET_ENEMY")};
            out[count++] = {0, kIconTargetParty, text::get("PARTY")};
            out[count++] = {0, kIconTargetAssist, text::get("VD_TOUCH_SMART_TARGET_ASSIST")};
            out[count++] = {0, kIconTargetPet, text::get("PET")};
            out[count++] = {0, kIconTargetSelf, text::get("YOU")};
        }
        return count;
    }
    const game::EntityStore& entities = frame.session->entities();
    const game::Entity* me = entities.find(frame.session->playerGuid());
    if (!me) {
        return 0;
    }
    const u64 current = frame.session->target();
    // The nearest enemy.
    std::array<EnemyCandidate, 64> candidates;
    const u32 enemies = enemyCandidates(frame, candidates);
    u32 nearest = enemies;
    for (u32 i = 0; i < enemies; ++i) {
        nearest = nearest == enemies || candidates[i].distance < candidates[nearest].distance ? i : nearest;
    }
    if (nearest < enemies) {
        out[count++] = {candidates[nearest].guid, kIconTargetEnemy, text::get("VD_TOUCH_SMART_TARGET_ENEMY")};
    }
    // The next member of the group in sight (after the one targeted).
    const game::GroupState& group = frame.session->play().social().group();
    const usize members = group.members.size();
    usize from = 0;
    for (usize i = 0; i < members; ++i) {
        from = group.members[i].guid == current ? i + 1 : from;
    }
    for (usize k = 0; k < members; ++k) {
        const game::GroupMember& m = group.members[(from + k) % members];
        if (m.guid != current && entities.find(m.guid)) {
            out[count++] = {m.guid, kIconTargetParty, text::get("PARTY")};
            break;
        }
    }
    // The target's target (the client's assist).
    const game::Entity* target = current ? entities.find(current) : nullptr;
    if (const u64 assist = target ? game::guidValue(*target, uf::UNIT_FIELD_TARGET) : 0;
        assist != 0 && assist != current && entities.find(assist)) {
        out[count++] = {assist, kIconTargetAssist, text::get("VD_TOUCH_SMART_TARGET_ASSIST")};
    }
    if (const u64 pet = game::guidValue(*me, uf::UNIT_FIELD_SUMMON); pet != 0 && entities.find(pet)) {
        out[count++] = {pet, kIconTargetPet, text::get("PET")};
    }
    out[count++] = {me->guid, kIconTargetSelf, text::get("YOU")};
    return count;
}

u32 Hud::reactionColor(const HudFrame& frame, const game::Entity& unit) const {
    const game::Entity* me = frame.session ? frame.session->entities().find(frame.session->playerGuid()) : nullptr;
    switch (me ? game::unitNameColor(*me, unit, frame.reactions) : game::NameColor::Neutral) {
    case game::NameColor::Hostile: return rgba(255, 0, 0);
    case game::NameColor::Friendly: return rgba(0, 255, 0);
    case game::NameColor::FriendlyPlayer: return rgba(64, 144, 255);
    case game::NameColor::Tapped: return rgba(128, 128, 128);
    default: return rgba(255, 255, 0);
    }
}

void Hud::namePlates(const HudFrame& frame) {
    m_labelTopCount = 0;
    if (!frame.session) {
        return;
    }
    VD_PROFILE_ZONE("Hud::namePlates");
    const u64 self = frame.session->playerGuid();
    const u64 target = frame.session->target();
    const game::Entity* me = frame.session->entities().find(self);
    // Phase 15 (options): the names by kind (UnitNamePlayer, UnitNameNPC, UnitNameOwn); the target's always (the
    // touch's hover: WoW.exe shows the unit under the cursor whatever the options).
    const HudFrame::NameOptions& names = frame.names;
    for (const Units::UnitView& view : frame.units) {
        if ((view.guid == self && !names.own) || glm::length(view.head - frame.camera) > 40.0) {
            continue;
        }
        const game::Entity* e = frame.session->entities().find(view.guid);
        f32 x, y;
        if (!e || !project(frame, view.head + glm::dvec3(0.0, 0.0, 0.3), x, y)) {
            continue;
        }
        UnitLabelView label;
        label.x = x;
        label.y = y;
        const net::CreatureQueryInfo* creature =
            e->type == ObjectType::Player ? nullptr
                                          : frame.session->creature(e->value(uf::OBJECT_FIELD_ENTRY));
        if (e->type == ObjectType::Player) {
            if (const std::string* found = frame.session->playerName(view.guid)) {
                label.name = *found;
            }
        } else if (creature) {
            label.name = creature->name;
        }
        if (label.name.empty()) {
            continue;
        }
        label.color = reactionColor(frame, *e);
        const u32 maxHealth = e->value(uf::UNIT_FIELD_MAXHEALTH);
        const u32 health = e->value(uf::UNIT_FIELD_HEALTH);
        label.health = maxHealth > 0 ? static_cast<f32>(health) / static_cast<f32>(maxHealth) : 1.0f;
        label.barColor = label.color;
        // The name plate (V: hostile and neutral units, Shift-V: friendly ones) of the living within 20
        // yards.
        const bool friendly = me && game::reactionTo(*me, *e, frame.reactions) == game::Reaction::Friendly;
        label.targeted = view.guid == target;
        label.plate = view.guid != self && !game::isDead(*e) && (friendly ? frame.friendPlates : frame.enemyPlates) &&
                      glm::length(view.feet - frame.position) <= 20.0;
        const bool isPlayer = e->type == ObjectType::Player;
        const bool named = view.guid == self ? names.own : view.guid == target || (isPlayer ? names.players : names.npcs);
        if (!label.plate && !named) {
            continue;
        }
        // UnitNamePlayerPVPTitle: the player's honor rank before its name (PLAYER_BYTES_3 byte 3: PVP_RANK_<rank>_<faction>
        // from Private / Scout, rank 5, up; GetPVPRankInfo).
        char titled[96];
        if (isPlayer && names.titles && !label.plate) {
            const u32 rank = e->byteValue(uf::PLAYER_BYTES_3, 3);
            if (rank >= 5 && rank <= 18) {
                const u32 race = e->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
                const bool alliance = race == 1 || race == 3 || race == 4 || race == 7;
                const bool female = e->byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1;
                char key[32];
                std::snprintf(key, sizeof(key), "PVP_RANK_%u_%u%s", rank, alliance ? 1u : 0u, female ? "_FEMALE" : "");
                const text::Key k{std::string_view(key)};
                if (text::has(k)) {
                    const std::string_view rankName = text::get(k);
                    std::snprintf(titled, sizeof(titled), "%.*s %.*s", static_cast<int>(rankName.size()), rankName.data(),
                                  static_cast<int>(label.name.size()), label.name.data());
                    label.name = titled;
                }
            }
        }
        char title[80];
        if (label.plate) {
            // UnitLevel's -1 ("??") and the level colour of the target frame (game/UnitText).
            const i32 shown = me ? game::shownLevel(*me, *e, creature ? creature->rank : 0, frame.reactions)
                                 : static_cast<i32>(e->value(uf::UNIT_FIELD_LEVEL));
            label.level = shown < 0 ? 0 : static_cast<u32>(shown);
            label.levelColor = shown < 0 ? rgba(255, 26, 26)
                               : me      ? game::levelColor(*me, *e, frame.reactions)
                                         : ui::kGold;
        } else {
            // Under the name: a player's guild (asked for once by id); an NPC's title; a pet's owner.
            if (const u32 guildId = isPlayer && names.guilds ? e->value(uf::PLAYER_GUILDID) : 0; guildId != 0) {
                label.title = frame.session->play().social().guildName(guildId);
            } else if (const u64 owner = creature ? game::guidValue(*e, uf::UNIT_FIELD_SUMMONEDBY) : 0;
                       owner != 0) {
                const game::Entity* master = frame.session->entities().find(owner);
                const std::string* ownerName =
                    master && master->type == ObjectType::Player ? frame.session->playerName(owner) : nullptr;
                constexpr u32 kCreatureTypeDemon = 3,
                              kCreatureTypeTotem = 11; // CreatureType (SharedDefines.h)
                if (ownerName && creature->type != kCreatureTypeTotem) {
                    label.title = text::format(title,
                                               creature->type == kCreatureTypeDemon ? "UNITNAME_TITLE_MINION"
                                                                                    : "UNITNAME_TITLE_PET",
                                               std::string_view(*ownerName));
                }
            } else if (creature) {
                label.title = creature->subname;
            }
        }
        // The quest givers' "!" and "?" are the client's models over the name (app/QuestMarks), risen while the name
        // shows: this fixed-size name keeps under them however far the unit is.
        f32 markX = 0.0f, markY = 0.0f;
        if (me && QuestMarks::markOf(frame.session->play(), *me, *e) != 0 &&
            project(frame, view.head + glm::dvec3(0.0, 0.0, QuestMarks::kRaisedBottom), markX, markY)) {
            label.y = std::max(label.y, markY + unitLabelHeight(m_draw, m_ui.dp(), label) + m_ui.px(2.0f));
        }
        const f32 top = drawUnitLabel(m_draw, m_ui.dp(), label);
        if (m_labelTopCount < m_labelTops.size()) {
            m_labelTops[m_labelTopCount++] = {view.guid, top};
        }
    }
    // Phase 13 (social): the raid target icons over their units, whatever the names' settings (WoW.exe draws
    // UI-RaidTargetingIcons with the unit's name object, PlayerName.cpp), above the name or plate when one shows.
    const game::PlayerCommands& marks = frame.session->play().commands();
    for (u8 icon = 1; icon <= net::kRaidTargetIcons; ++icon) {
        const u64 guid = marks.raidTargetUnit(icon);
        for (const Units::UnitView& view : guid != 0 ? frame.units : std::span<const Units::UnitView>{}) {
            f32 x, y;
            if (view.guid != guid || glm::length(view.head - frame.camera) > 40.0 ||
                !project(frame, view.head + glm::dvec3(0.0, 0.0, 0.3), x, y)) {
                continue;
            }
            for (u32 i = 0; i < m_labelTopCount; ++i) {
                y = m_labelTops[i].guid == guid ? m_labelTops[i].top : y;
            }
            const f32 size = m_ui.px(26.0f);
            drawRaidTargetIcon(m_draw, {x - size * 0.5f, y - size - m_ui.px(2.0f), size, size}, icon);
            break;
        }
    }
    chatBubbles(frame);
}

void Hud::chatBubbles(const HudFrame& frame) {
    if (!frame.bubbles || !frame.session || frame.bubbles->count() == 0) {
        return;
    }
    VD_PROFILE_ZONE("Hud::chatBubbles");
    constexpr f64 kBubbleYards = 60.0;
    struct Shown {
        const ChatBubbles::Bubble* bubble;
        f32 x, y;
        f64 distance;
    };
    std::array<Shown, ChatBubbles::kMax> shown;
    u32 count = 0;
    for (const ChatBubbles::Bubble& b : frame.bubbles->bubbles()) {
        const Units::UnitView* view = nullptr;
        for (const Units::UnitView& v : b.speaker != 0 ? frame.units : std::span<const Units::UnitView>{}) {
            if (v.guid == b.speaker) {
                view = &v;
                break;
            }
        }
        f32 x, y;
        if (!view || glm::length(view->head - frame.camera) > kBubbleYards ||
            !project(frame, view->head + glm::dvec3(0.0, 0.0, 0.3), x, y)) {
            continue;
        }
        // The tail points at the top of the speaker's name (and title, quest mark), or just over its head.
        y -= m_ui.px(4.0f);
        for (u32 i = 0; i < m_labelTopCount; ++i) {
            if (m_labelTops[i].guid == b.speaker) {
                y = m_labelTops[i].top - m_ui.px(1.0f);
                break;
            }
        }
        shown[count++] = {&b, x, y, glm::length(view->head - frame.camera)};
    }
    // The farthest first: the nearest speakers' bubbles cover theirs.
    std::sort(shown.begin(), shown.begin() + count,
              [](const Shown& a, const Shown& b) { return a.distance > b.distance; });
    const u32 nowMs = static_cast<u32>(frame.nowMs);
    for (u32 i = 0; i < count; ++i) {
        const ChatBubbles::Bubble& b = *shown[i].bubble;
        const std::string_view text = b.view();
        const ChatBubbleLayout layout =
            chatBubbleLayout(m_draw, u(1.0f), shown[i].x, shown[i].y, text, frame.safeArea);
        drawChatBubble(m_draw, u(1.0f), layout, text, b.color, ChatBubbles::alpha(b, nowMs));
    }
}

void Hud::contextLabel(const HudFrame& frame, const Rect& button) {
    // Parity: what the context button acts on, as the client's cursor and tooltip say over it — the name
    // above the button (small, white and outlined like an action button's name), the cursor's icon on its top
    // right corner.
    if (frame.context == HudContextAction::None) {
        return;
    }
    if (frame.contextName.empty()) {
        if (frame.contextIcon != 0) {
            const f32 size = button.w * 0.42f;
            m_draw.sprite({button.right() - size * 0.8f, button.y - size * 0.2f, size, size},
                          frame.contextIcon);
        }
        return;
    }
    // A strip over the button: the cursor's icon, then the name, at most one and a half buttons wide
    // (shortened with "..." past that), kept on screen.
    ui::TextStyle style;
    style.size = m_ui.px(11.0f);
    style.color = ui::kWhite;
    style.outline = std::max(1.0f, m_ui.px(1.0f));
    const f32 height = style.size * 1.5f;
    const f32 icon = frame.contextIcon != 0 ? height : 0.0f;
    const f32 pad = m_ui.px(3.0f);
    const f32 textW = std::min(m_draw.measure(frame.contextName, style.font, style.size) + 1.0f,
                               button.w * 1.6f - icon - 2.0f * pad);
    Rect r{button.centerX() - (icon + textW + 2.0f * pad) * 0.5f, button.y - height - m_ui.px(2.0f),
           icon + textW + 2.0f * pad, height};
    r.x = std::min(r.x, frame.safeArea.right() - r.w - m_ui.px(2.0f));
    m_draw.solid(r, rgba(0, 0, 0, 110));
    if (icon > 0.0f) {
        m_draw.sprite({r.x + pad * 0.5f, r.y, icon, icon}, frame.contextIcon);
    }
    ui::Gui g{m_ui, m_draw};
    ui::label(g, {r.x + pad + icon, r.y, textW, r.h}, frame.contextName, style);
}

void Hud::combatText(u64 guid, std::string_view text, u32 color, bool big, u64 nowMs, HudWorldText kind) {
    FloatingText& f = m_floating[m_floatingNext];
    m_floatingNext = (m_floatingNext + 1) % static_cast<u32>(m_floating.size());
    f.guid = guid;
    f.startMs = nowMs;
    f.color = color;
    f.big = big;
    f.kind = kind;
    f.drift = static_cast<f32>(static_cast<i32>(m_floatingNext % 5) - 2) * 0.35f;
    const usize length = std::min(text.size(), f.text.size() - 1);
    std::memcpy(f.text.data(), text.data(), length);
    f.text[length] = '\0';
}

void Hud::floatingTexts(const HudFrame& frame) {
    constexpr f32 kLifeMs = 1500.0f;
    // World text types 4 and 5 (0xCE8898, 0xCE88B4): alpha up for 500 ms, whole to 2000, down to 4500; they
    // do not rise (0 yards) nor grow.
    constexpr f32 kNoticeInMs = 500.0f, kNoticeOutMs = 2000.0f, kNoticeLifeMs = 4500.0f;
    constexpr f64 kFirstPerson = 1.8315; // 0x50D390: the camera this close to its unit shows no text over it
    const u64 player = frame.session ? frame.session->playerGuid() : 0;
    for (FloatingText& f : m_floating) {
        if (f.guid == 0) {
            continue;
        }
        const bool notice = f.kind == HudWorldText::Notice;
        const f32 life = notice ? kNoticeLifeMs : kLifeMs;
        const f32 age = static_cast<f32>(m_ui.nowMs() - f.startMs);
        if (age >= life || m_ui.nowMs() < f.startMs) {
            f.guid = 0;
            continue;
        }
        const Units::UnitView* view = nullptr;
        for (const Units::UnitView& v : frame.units) {
            if (v.guid == f.guid) {
                view = &v;
                break;
            }
        }
        f32 x, y;
        if (!view || !project(frame, view->head + glm::dvec3(0.0, 0.0, 0.5), x, y)) {
            continue;
        }
        if (f.guid == player && glm::length(frame.camera - view->head) < kFirstPerson) {
            continue; // 0x6C7840: not over the player seen in first person
        }
        const f32 t = age / life;
        ui::TextStyle style;
        style.font = ui::FontId::Friz; // Fonts.xml's DAMAGE_TEXT_FONT (FRIZQT__), the engine's damage numbers
        style.outline = std::max(1.0f, m_ui.px(1.2f));
        style.align = ui::Align::Center;
        if (notice) {
            // The height of the damage numbers (every type but 2: 0.0183 of the screen).
            style.size = m_ui.px(18.0f);
            style.color = withAlpha(f.color, age < kNoticeInMs ? age / kNoticeInMs
                                             : age < kNoticeOutMs
                                                 ? 1.0f
                                                 : 1.0f - (age - kNoticeOutMs) / (life - kNoticeOutMs));
            m_draw.text(x, y, f.text.data(), style);
            continue;
        }
        style.size = m_ui.px(f.big ? 26.0f : 18.0f) * (t < 0.1f ? 0.7f + 3.0f * t : 1.0f); // pops in
        style.color = withAlpha(f.color, t < 0.7f ? 1.0f : 1.0f - (t - 0.7f) / 0.3f);
        m_draw.text(x + f.drift * m_ui.px(40.0f) * t, y - m_ui.px(60.0f) * t, f.text.data(), style);
    }
}

} // namespace vd
