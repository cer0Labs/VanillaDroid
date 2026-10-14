#include "app/PlayerLook.h"

#include "app/GameSession.h"
#include "net/UpdateFields.gen.h"

namespace vd {

namespace uf = net::uf;

bool playerCharacterLook(GameSession& session, const game::Entity& e, game::CharacterLook& c) {
    constexpr u32 kVisibleItemStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;
    c = game::CharacterLook{};
    c.race = e.byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    c.gender = e.byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    c.skin = e.byteValue(uf::PLAYER_BYTES, 0);
    c.face = e.byteValue(uf::PLAYER_BYTES, 1);
    c.hairStyle = e.byteValue(uf::PLAYER_BYTES, 2);
    c.hairColor = e.byteValue(uf::PLAYER_BYTES, 3);
    c.facialHair = e.byteValue(uf::PLAYER_BYTES_2, 0);
    bool complete = true;
    for (u32 slot = 0; slot < game::kEquipSlots; ++slot) {
        const u32 entry = e.value(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride));
        if (entry == 0) {
            continue;
        }
        if (const net::ItemQueryInfo* item = session.item(entry)) {
            c.items[slot] = item->displayId;
            c.inventoryTypes[slot] = static_cast<u8>(item->inventoryType);
            c.sheaths[slot] = static_cast<u8>(item->sheath);
        } else {
            complete = false; // asked for
        }
    }
    if (const game::GuildEmblem* emblem = session.play().social().guildEmblem(e.value(uf::PLAYER_GUILDID))) {
        c.emblem = *emblem;
    }
    return complete;
}

} // namespace vd
