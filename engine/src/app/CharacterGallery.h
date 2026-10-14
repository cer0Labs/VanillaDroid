#pragma once

#include "app/CharacterWindows.h"
#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/ModelFrame.h"
#include "core/Types.h"
#include "game/Abilities.h"
#include "game/AbilityData.h"
#include "game/Entities.h"
#include "game/FactionList.h"
#include "game/PaperDoll.h"
#include "game/SkillList.h"

#include <array>
#include <vector>

namespace vd {

class ModelFrameGallery;

// Development (phase 15): the character window and the InspectFrame of app/CharacterWindows with a level-12
// human warrior's sample data, for UiGallery's pages character (the paper doll), reputation, skills, honor,
// inspect and inspecthonor. The numbers and tooltips come from game::PaperDoll over a sample player's update
// fields, the lists from game::FactionList / game::SkillList over sample tables, so the pages show what the
// game would; the window's texts come from the texts table (VD_LOCALE=esES), the sample data (names) is
// English. The 3D models need a map
// (--map Azeroth --pos -9050,440: app/ModelFrameGallery's samples). What the player does goes to the log.
class CharacterGallery {
public:
    enum class Page : u8 { Character, Reputation, Skills, Honor, Inspect, InspectHonor };

    void setModels(ModelFrameGallery* models) { m_models = models; }
    void draw(DialogContext& c, Page page);

private:
    void build();
    void paperDoll(DialogContext& c, bool inspect, u32 firstTab);
    void tabBody(DialogContext& c, u32 tab, const CharacterPageFrame& frame);
    void honor(DialogContext& c, const CharacterPageFrame& frame, bool withLevel);

    ModelFrameGallery* m_models = nullptr;
    bool m_built = false;
    Page m_page = Page::Character;
    u32 m_tab = 0;
    u32 m_inspectTab = 0;
    ItemDrag m_drag;
    std::vector<ItemSlotView> m_equipment;
    game::Entity m_player;
    game::PaperDoll m_doll;
    std::array<PaperDollStatView, game::kPaperDollStats> m_stats{};
    ModelFrameState m_model;
    ModelFrameState m_inspectModel;
    i32 m_statShown = -1;
    // Reputation.
    std::vector<game::FactionInfo> m_factionInfo;
    game::ReputationList m_reputation;
    game::FactionList m_factions;
    std::array<FactionRowView, 32> m_factionViews{};
    bool m_factionDetail = true;
    i32 m_reputationTip = -1;
    // Skills.
    std::vector<game::SkillLineSource> m_skillSources;
    game::SkillList m_skills;
    std::array<SkillLineView, 48> m_skillViews{};
    std::array<std::array<char, 40>, 48> m_skillRanks{};
    std::array<char, 512> m_skillDescription{};
    i32 m_skillTip = -1;
    // Honor.
    i32 m_honorTip = -1;
};

} // namespace vd
