#include "app/PvpScreens.h"

#include "app/CombatScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/SocialScreens.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Honor.h"
#include "game/Notices.h"
#include "net/UpdateFields.gen.h"

#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;

} // namespace

PvpScreens::PvpScreens() = default;

void PvpScreens::readDemo() {
    // On the first offline frame, not in the constructor: Android sets the intent's switches after Game exists.
    m_demoRead = true;
    const char* demo = VD_DEV_ENV("VD_PVP_DEMO");
    if (!demo || !*demo) {
        return;
    }
    m_demo = static_cast<u8>(
        (std::strstr(demo, "spirit") ? kDemoSpirit : 0) | (std::strstr(demo, "skinned") ? kDemoSkinned : 0) |
        (std::strstr(demo, "credit") ? kDemoCredit : 0) | (std::strstr(demo, "attack") ? kDemoAttack : 0));
    VD_LOG_INFO("VD_PVP_DEMO=%s (spirit, skinned, credit, attack)", demo);
}

void PvpScreens::update(GameSession& session, Hud& hud, u32 nowMs) {
    VD_PROFILE_ZONE("PvpScreens::update");
    game::Gameplay& play = session.play();
    if (m_player != session.playerGuid()) { // another character: its events from now on
        m_player = session.playerGuid();
        m_eventCursor = play.events().end();
    }
    const game::Entity* me = session.entities().find(m_player);
    const bool female = me && me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1;
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind != game::GameEventKind::HonorGained) {
            continue;
        }
        // 0x6072E0 (kills only): "HK: <rank>" / "DK: <rank>" over the player, world text type 5 (224 202 10).
        char line[96];
        const std::string_view text = game::formatHonorWorldText(line, *e, female);
        if (!text.empty()) {
            const game::NoticeColor c = game::noticeColor(game::NoticeKind::HonorGain);
            hud.combatText(m_player, text, ui::rgba(c.r, c.g, c.b), false, nowMs, HudWorldText::Notice);
        }
    }
}

void PvpScreens::drawOffline(DialogContext& c, CombatScreens& combat, SocialScreens& social, u32 nowMs) {
    if (!m_demoRead) {
        readDemo();
    }
    if (m_demo == 0) {
        return;
    }
    VD_PROFILE_ZONE("PvpScreens::drawOffline");
    if (!m_demoLines) { // once: the sample lines where the game puts them
        m_demoLines = true;
        if (m_demo & kDemoCredit) {
            for (const PvpGallery::Line& l : PvpGallery::sampleLines(true)) {
                combat.logLine(l.text, l.color);
            }
        }
        if (m_demo & kDemoAttack) {
            for (const PvpGallery::Line& l : PvpGallery::sampleLines(false)) {
                social.systemNotice(l.color, l.text);
            }
        }
    }
    if (m_demo & kDemoCredit) {
        m_gallery.drawHonorTexts(c, nowMs);
    }
    if (m_demo & kDemoSkinned) {
        m_gallery.drawSkinned(c, social.chatRect(), nowMs);
    }
    if (m_demo & kDemoSpirit) {
        m_gallery.drawSpiritHealer(c, nowMs);
    }
}

} // namespace vd
