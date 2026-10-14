#include "app/QuestMarks.h"

#include "app/GameSession.h"
#include "app/Units.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "game/Appearance.h"
#include "game/UnitInfo.h"
#include "world/Actors.h"

#include <array>

namespace vd {
namespace {

// WoW.exe's mark models (0x5FA430: the table at 0xC4D9D8; TalkToMeBlue is only an innkeeper's, from code nothing calls).
constexpr std::array<u64, 8> kModels = {0,
                                        assetPathHash("Interface\\Buttons\\TalkToMe.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeQuestion_Grey.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeGrey.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeGreen.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeBlue.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeQuestionMark.m2"),
                                        assetPathHash("Interface\\Buttons\\TalkToMeQuestion_LTBlue.m2")};
constexpr u8 kTaxiModel = 4;
// QuestGiverStatus -> model (0x80C454): none, unavailable grey "!", chat none, incomplete grey "?", reputation reward
// light blue "?", available "!", the two rewards "?".
constexpr std::array<u8, 8> kStatusModel = {0, 3, 0, 2, 7, 1, 6, 6};
constexpr u16 kStand = 0, kStandHigh = 190; // AnimationData.dbc

} // namespace

u8 QuestMarks::markOf(const game::Gameplay& play, const game::Entity& me, const game::Entity& e) {
    if (e.type != game::ObjectType::Unit || game::isDead(e)) {
        return 0;
    }
    const u32 flags = game::npcFlags(e);
    u8 model = 0;
    if ((flags & game::kNpcFlagFlightMaster) && play.travel().taxiStatus(e.guid) == game::TaxiNodeStatus::NotLearned) {
        model = kTaxiModel; // 0x5ECDD0 overwrites whatever the unit shows
    } else if (flags & game::kNpcFlagQuestGiver) {
        const u32 status = static_cast<u32>(play.questGiverStatus(e.guid));
        model = status < kStatusModel.size() ? kStatusModel[status] : 0;
    }
    return model != 0 && game::reactionTo(me, e, play.reactions()) != game::Reaction::Hostile ? model : 0;
}

void QuestMarks::update(GameSession& session, const Units& units, Actors& actors, NameShown nameShown,
                        const void* context) {
    VD_PROFILE_ZONE("QuestMarks::update");
    ++m_frame;
    if (!session.inWorld()) {
        clear(actors);
        return;
    }
    const game::Gameplay& play = session.play();
    const game::Entity* me = session.entities().find(session.playerGuid());
    session.entities().forEach([&](const game::Entity& e) {
        const u8 model = me ? markOf(play, *me, e) : 0;
        const u32 parent = model != 0 ? units.actorOf(e.guid) : ~0u;
        if (model == 0 || parent == ~0u) {
            return;
        }
        Mark& mark = m_marks[e.guid];
        mark.seen = m_frame;
        // A rider's own model has its name point higher (0x6074C0).
        const u32 attachment = actors.parent(parent) != Actors::kNone ? game::attach::kNameMounted : game::attach::kName;
        if (mark.actor == ~0u || mark.model != model) {
            if (mark.actor != ~0u) {
                actors.destroy(mark.actor);
            }
            mark = {};
            mark.seen = m_frame;
            mark.model = model;
            mark.actor = actors.create(true); // drawn only with its unit
            game::ActorLook look;
            look.model = kModels[model];
            actors.setLook(mark.actor, look);
        }
        if (mark.parent != parent || mark.attachment != attachment) {
            mark.parent = parent;
            mark.attachment = attachment;
            actors.attach(mark.actor, parent, attachment);
        }
        const u16 animation = nameShown && nameShown(context, e.guid) ? kStandHigh : kStand;
        if (animation != mark.animation) {
            mark.animation = animation;
            actors.play(mark.actor, animation, true, 1.0f);
        }
    });
    m_gone.clear();
    for (const auto& [guid, mark] : m_marks) {
        if (mark.seen != m_frame) {
            m_gone.push_back(guid);
        }
    }
    for (const u64 guid : m_gone) {
        actors.destroy(m_marks[guid].actor);
        m_marks.erase(guid);
    }
}

void QuestMarks::clear(Actors& actors) {
    for (const auto& [guid, mark] : m_marks) {
        actors.destroy(mark.actor);
    }
    m_marks.clear();
}

} // namespace vd
