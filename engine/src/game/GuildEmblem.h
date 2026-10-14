#pragma once

#include "core/Types.h"

#include <array>

namespace vd::game {

// Phase 15 (guild): a guild's tabard design, as SMSG_GUILD_QUERY_RESPONSE and MSG_SAVE_GUILD_EMBLEM carry it:
// the emblem (its style and colour), the border (style and colour) and the background's colour. -1: not
// designed.
struct GuildEmblem {
    i32 emblemStyle = -1, emblemColor = -1, borderStyle = -1, borderColor = -1, backgroundColor = -1;

    // Every choice within what the designer offers (WoW.exe 0x5E03F0 refuses anything else).
    bool designed() const;
    i32 choice(u32 index) const;
    void setChoice(u32 index, i32 value);
    bool operator==(const GuildEmblem&) const = default;
};

// TabardFrameCustomization1..5 (EMBLEM_SYMBOL, EMBLEM_SYMBOL_COLOR, EMBLEM_BORDER, EMBLEM_BORDER_COLOR,
// EMBLEM_BACKGROUND): how many choices each arrow cycles through (CycleVariation, WoW.exe 0x808220). The
// client's Textures\GuildEmblems hold that many: 170 emblems in 17 colours, borders 0..5 in 17 colours (6..9
// exist in 4 colours only, never offered), 51 backgrounds.
constexpr u32 kEmblemChoiceKinds = 5;
constexpr std::array<i32, kEmblemChoiceKinds> kEmblemChoiceCounts = {170, 17, 6, 17, 51};

// GetTabardCreationCost: 10 gold (vMaNGOS HandleSaveGuildEmblemOpcode takes 10 * GOLD).
constexpr u32 kTabardCreationCost = 10 * 100 * 100;
// A tabard whose ItemDisplayInfo has this flag shows the wearer's guild emblem (WoW.exe 0x5D648A: the tabard
// slot's display, Flags & 1; the Guild Tabard, item 5976, is display 20621).
constexpr u32 kGuildTabardDisplayFlag = 0x1;
constexpr u32 kGuildTabardDisplay = 20621;

// The textures.vdpk assets of a design for the upper or the lower torso region of the character skin
// (Textures\GuildEmblems\Background_%02d_TU_U.blp, Border_%02d_%02d_..., Emblem_%02d_%02d_..., WoW.exe
// 0x47A4D0 / 0x47A520 / 0x47A570), each region sized like the item texture components. They replace the
// tabard's own layer as the background, the border and the emblem over each other (WoW.exe 0x47A610: layers
// 2, 3 and 4 of the region).
struct EmblemTextures {
    u64 background = 0, border = 0, emblem = 0;
};
EmblemTextures emblemTextures(const GuildEmblem& emblem, bool upper);

} // namespace vd::game
