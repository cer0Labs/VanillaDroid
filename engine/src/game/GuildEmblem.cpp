#include "game/GuildEmblem.h"

#include "core/Hash.h"

#include <cstdio>

namespace vd::game {

bool GuildEmblem::designed() const {
    for (u32 i = 0; i < kEmblemChoiceKinds; ++i) {
        const i32 v = choice(i);
        if (v < 0 || v >= kEmblemChoiceCounts[i]) {
            return false;
        }
    }
    return true;
}

i32 GuildEmblem::choice(u32 index) const {
    switch (index) {
    case 0: return emblemStyle;
    case 1: return emblemColor;
    case 2: return borderStyle;
    case 3: return borderColor;
    case 4: return backgroundColor;
    default: return -1;
    }
}

void GuildEmblem::setChoice(u32 index, i32 value) {
    switch (index) {
    case 0: emblemStyle = value; break;
    case 1: emblemColor = value; break;
    case 2: borderStyle = value; break;
    case 3: borderColor = value; break;
    case 4: backgroundColor = value; break;
    default: break;
    }
}

EmblemTextures emblemTextures(const GuildEmblem& emblem, bool upper) {
    EmblemTextures out;
    if (!emblem.designed()) {
        return out;
    }
    const char* region = upper ? "TU" : "TL";
    char path[96];
    std::snprintf(path, sizeof(path), "Textures\\GuildEmblems\\Background_%02d_%s_U.blp",
                  emblem.backgroundColor, region);
    out.background = assetPathHash(path);
    std::snprintf(path, sizeof(path), "Textures\\GuildEmblems\\Border_%02d_%02d_%s_U.blp", emblem.borderStyle,
                  emblem.borderColor, region);
    out.border = assetPathHash(path);
    std::snprintf(path, sizeof(path), "Textures\\GuildEmblems\\Emblem_%02d_%02d_%s_U.blp", emblem.emblemStyle,
                  emblem.emblemColor, region);
    out.emblem = assetPathHash(path);
    return out;
}

} // namespace vd::game
