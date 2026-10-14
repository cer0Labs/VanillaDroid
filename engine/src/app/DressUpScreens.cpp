#include "app/DressUpScreens.h"

#include "app/GameSession.h"
#include "app/ModelViews.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/TryOn.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd {
namespace {

namespace uf = net::uf;
constexpr u32 kEffectDualWield =
    40; // SPELL_EFFECT_DUAL_WIELD: WoW.exe keeps the spell that taught it (0xC4D770)

} // namespace

void DressUpScreens::closeRoom(Room& room) {
    const ModelFrameState frame = room.frame;
    room = Room{};
    room.frame = frame;
}

void DressUpScreens::openRoom(GameSession& session, Room& room) {
    (void)session;
    room.open = true;
    room.based = false; // SetUnit("player")
    room.triedCount = 0;
    room.dirty = true;
}

void DressUpScreens::tryOn(GameSession& session, u32 entry) {
    if (!session.inWorld() || entry == 0) {
        return;
    }
    // Blizzard_AuctionDressUp.lua replaces DressUpItem: with the auction house shown, its own dressing room.
    Room& room = session.play().exchange().auction().open() ? m_rooms[1] : m_rooms[0];
    if (!room.open) {
        openRoom(session, room);
    }
    if (room.triedCount == room.tried.size()) { // the oldest goes (later ones cover it anyway)
        std::copy(room.tried.begin() + 1, room.tried.end(), room.tried.begin());
        --room.triedCount;
    }
    room.tried[room.triedCount++] = entry;
    room.dirty = true;
}

bool DressUpScreens::canDualWield(GameSession& session) const {
    if (!m_data) {
        return false;
    }
    for (const u32 known : session.play().spells().known()) {
        if (const std::optional<game::SpellInfo> spell = m_data->spell(known)) {
            for (const u32 effect : spell->effects) {
                if (effect == kEffectDualWield) {
                    return true;
                }
            }
        }
    }
    return false;
}

void DressUpScreens::rebuild(GameSession& session, Room& room) {
    const bool answers = room.incomplete && room.queryVersion != session.queryVersion();
    if (!room.dirty && !answers) {
        return;
    }
    const game::Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    bool complete = true;
    if (!room.based) {
        room.based = game::characterLookOf(*me, session.play().queries(), room.base);
        complete = room.based;
    }
    game::CharacterLook look = room.base;
    const bool dual = canDualWield(session);
    for (u32 i = 0; i < room.triedCount; ++i) {
        const net::ItemQueryInfo* info = session.item(room.tried[i]);
        if (!info) {
            complete = false;
            continue;
        }
        game::tryOn(look,
                    {info->displayId, static_cast<u8>(info->inventoryType), static_cast<u8>(info->sheath)},
                    dual);
    }
    if (m_appearance && m_appearance->ready()) {
        room.look =
            m_appearance->character(look, false); // the weapons in the hands, as the paper doll's model
    }
    room.queryVersion = session.queryVersion();
    room.dirty = false;
    room.incomplete = !complete;
}

void DressUpScreens::update(GameSession& session) {
    VD_PROFILE_ZONE("DressUpScreens::update");
    if (m_rooms[1].open && (!session.inWorld() || !session.play().exchange().auction().open())) {
        closeRoom(m_rooms[1]); // AuctionFrame_OnHide: HideUIPanel(AuctionDressUpFrame)
    }
    if (!session.inWorld()) {
        closeRoom(m_rooms[0]);
    }
    for (u32 i = 0; i < m_rooms.size(); ++i) {
        // OnShow / OnHide: igCharacterInfoOpen / igCharacterInfoClose.
        if (m_rooms[i].open != m_wasOpen[i]) {
            m_uiSound(m_rooms[i].open ? UiSound::CharacterOpen : UiSound::CharacterClose);
            m_wasOpen[i] = m_rooms[i].open;
        }
        if (m_rooms[i].open && session.inWorld()) {
            rebuild(session, m_rooms[i]);
        }
    }
}

DressUpView DressUpScreens::view(GameSession& session, Room& room, u32 viewId) {
    DressUpView v;
    v.title = text::get("DRESSUP_FRAME");
    v.instructions = text::get("VD_DRESSUP_INSTRUCTIONS");
    const game::Entity* me = session.entities().find(session.playerGuid());
    const u8 race = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 0) : 0;
    for (u32 i = 0; i < 4; ++i) {
        v.backgrounds[i] = dressUpBackground(race, i + 1); // SetDressUpBackground
    }
    if (m_views && room.look.model != 0) {
        v.model = m_views->show(viewId == 0 ? ModelViewId::DressUp : ModelViewId::AuctionDressUp, room.look,
                                room.frame.facing);
    }
    return v;
}

void DressUpScreens::act(GameSession& session, Room& room, const DressUpResult& r) {
    (void)session;
    if (r.rotated) {
        m_uiSound(UiSound::InventoryRotate); // Model_RotateLeft / Right
    }
    if (r.reset) { // DressUpModel:Dress(): as the player is now
        room.based = false;
        room.triedCount = 0;
        room.dirty = true;
        m_uiSound(UiSound::TitleOptionOk);
    }
    if (r.closed) {
        closeRoom(room);
    }
}

void DressUpScreens::draw(GameSession& session, DialogContext& c) {
    Room& room = m_rooms[0];
    if (!room.open || !session.inWorld()) {
        return;
    }
    const DressUpView v = view(session, room, 0);
    act(session, room, dressUpWindow(c, v, room.frame));
}

void DressUpScreens::drawAuction(GameSession& session, DialogContext& c, const ui::Rect& place) {
    Room& room = m_rooms[1];
    if (!room.open || !session.inWorld()) {
        return;
    }
    DressUpView v = view(session, room, 1);
    v.instructions = {};
    act(session, room, auctionDressUpWindow(c, place, v, room.frame));
}

f32 DressUpScreens::auctionDockWidth(const DialogContext& c, f32 width) {
    return std::min(width * 0.3f, c.ui.px(230.0f));
}

} // namespace vd
