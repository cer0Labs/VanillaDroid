#pragma once

#include "app/Dialogs.h"
#include "app/GuildWindows.h"
#include "app/ModelFrame.h"
#include "app/SocialWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Appearance.h"
#include "game/GuildControl.h"
#include "game/GuildNotice.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

namespace game {
class GameData;
class SocialData;
} // namespace game
namespace ui {
class TextInput;
}

class GameSession;
class ModelViews;
class PopupScreens;
class SocialScreens;
class UiResources;
class UnitMenuScreens;

// Phase 15 (guild): the game's side of the guild windows (app/GuildWindows): the social window's guild tab
// (GuildFrame with GuildMemberDetailFrame, GuildInfoFrame and GuildControlPopupFrame beside it), the
// registrar (GuildRegistrarFrame), the charter (PetitionFrame) and the tabard designer (TabardFrame with the
// player's model wearing the design), filled from game::Social, GuildControl, Petitions and TabardDesigner,
// and the lines their answers show (the UIErrorsFrame). GameScreens owns it; SocialScreens draws the tab
// through it; the panels are UiPanel::GuildRegistrar / Petition / Tabard. Main thread; the views are rebuilt
// into reserved storage (no allocation per frame once warmed up).
class GuildScreens {
public:
    void init(const game::GameData* data, UiResources* resources);
    // Where the social tables come from (zone and class names: SocialScreens::socialData).
    void setSocialScreens(const SocialScreens* social) { m_socialScreens = social; }
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    void setUnitMenus(UnitMenuScreens* menus) { m_unitMenus = menus; }
    void setModelViews(ModelViews* views) { m_modelViews = views; }
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void setMessageSink(MessageSink sink, void* context) {
        m_message = sink;
        m_messageContext = context;
    }

    // Once a frame in the world: the notices of the charter and the tabard designer (UIErrorsFrame lines).
    void update(GameSession& session, u32 nowMs);
    void leaveWorld();

    // The guild tab's page on FriendsFrame's GuildFrame art (SocialScreens draws the frame and its tabs), then, after
    // the tabs, the side frame up (member detail, information, control) right of the frame.
    void drawTab(GameSession& session, DialogContext& c, ui::TextInput& text, const FriendsFrameShell& f);
    void drawSideFrame(GameSession& session, DialogContext& c, ui::TextInput& text, const FriendsFrameShell& f);

    // The panels (GameScreens: panelWanted / closePanel / drawPanel).
    static bool registrarOpen(const GameSession& session);
    static bool petitionOpen(const GameSession& session);
    static bool tabardOpen(const GameSession& session);
    void closeRegistrar(GameSession& session);
    void closePetition(GameSession& session);
    void closeTabard(GameSession& session);
    void drawRegistrar(GameSession& session, DialogContext& c, ui::TextInput& text);
    void drawPetition(GameSession& session, DialogContext& c);
    void drawTabard(GameSession& session, DialogContext& c);

private:
    enum class Side : u8 { None, Member, Info, Control };
    enum class Menu : u8 { None, Rank };

    void buildTab(GameSession& session);
    void drawSide(GameSession& session, DialogContext& c, ui::TextInput& text, const ui::Rect& area);
    void memberFrame(GameSession& session, DialogContext& c, const ui::Rect& area);
    void infoFrame(GameSession& session, DialogContext& c, ui::TextInput& text, const ui::Rect& area);
    void controlFrame(GameSession& session, DialogContext& c, ui::TextInput& text, const ui::Rect& area);
    void selectRank(GameSession& session, u32 rank); // GuildControlSetRank + the check boxes and the label
    void notice(GameSession& session, const game::GuildNotice& n);
    std::string_view zoneName(u32 zone) const;
    std::string_view className(u32 cls) const;
    static std::string_view zoneOf(const void* self, u32 zone);
    static std::string_view classOf(const void* self, u32 cls);
    void sound(UiSound s) const;

    const game::GameData* m_data = nullptr;
    const SocialScreens* m_socialScreens = nullptr;
    UiResources* m_resources = nullptr;
    PopupScreens* m_popups = nullptr;
    UnitMenuScreens* m_unitMenus = nullptr;
    ModelViews* m_modelViews = nullptr;
    UiSoundSink m_uiSound;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;
    bool m_namesSet = false;

    // The guild tab.
    Side m_side = Side::None;
    u64 m_lastTabMs = 0;        // FriendsFrame_OnHide: a tab not drawn for a while starts over
    bool m_guildStatus = false; // FriendsFrame.playerStatusFrame = nil
    u32 m_rosterVersion = ~0u;
    u32 m_rosterSorted = 0;
    std::vector<GuildRowView> m_rows;
    std::vector<u32> m_rowMembers; // the members behind the rows
    std::vector<std::array<std::string, kGuildColumns>> m_cells;
    std::string m_totals, m_onlineTotals, m_motd;
    // The member detail.
    std::string m_levelClass, m_zone, m_online, m_note, m_officerNote;
    // GuildInfoFrame: the text being edited.
    std::string m_info;
    bool m_infoDirty = false;
    // GuildControlPopupFrame (GuildControlSetRank's rank, its check boxes and label as edited, Accept
    // enabled).
    u32 m_controlRank = 0;
    std::array<bool, game::kGuildControlOptions> m_flags{};
    std::string m_rankLabel;
    bool m_controlChanged = false;
    Menu m_menu = Menu::None;
    std::vector<ChoiceView> m_choices;

    // GuildRegistrarFrame.
    bool m_registrarPurchase = false;
    u64 m_registrarNpc = 0;
    std::string m_charterName;
    // PetitionFrame.
    std::string m_petitionTitle, m_petitionMaster;
    std::array<std::string, 9> m_signers{};
    // TabardFrame: the player wearing the design (rebuilt when the design or what it wears changes).
    ModelFrameState m_tabardModel;
    u64 m_tabardVendor = 0;
    game::CharacterLook m_tabardCharacter;
    game::ActorLook m_tabardLook;
    bool m_tabardLookBuilt = false;

    // Notices.
    u32 m_petitionCursor = 0, m_tabardCursor = 0;
    u64 m_cursorsPlayer = 0;
};

} // namespace vd
