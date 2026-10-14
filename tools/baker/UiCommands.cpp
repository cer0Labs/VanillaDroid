// ui.vdpk (phase 7): the interface textures of the touch UI in one atlas, the client's fonts as distance fields and
// the minimap tile tables. See pack/UiFormat.h and docs/decisions/ADR-0002-ui.md. Phase 22 (apppack): only what comes
// from the player's clients; what is ours (texts, art, font) goes into app.vdpk (AppCommands.cpp).

#include "BakerCommon.h"

#include "bake/PackWriter.h"
#include "bake/TextureBaker.h"
#include "bake/UiBaker.h"
#include "bake/UiStrings.h" // phase 22 (apppack)
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Blp.h"
#include "formats/LuaStrings.h"
#include "formats/Wmo.h"
#include "pack/UiFormat.h"
#include "pack/UiModelFormat.h" // retouches (ui models)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

// The client textures the UI draws. A trailing '*' takes every texture of that directory starting with the prefix.
constexpr const char* kUiTextures[] = {
    // Unit frames
    "Interface\\TargetingFrame\\UI-TargetingFrame.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-Elite.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-Rare.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-RareMob.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-NoMana.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-LevelBackground.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp",
    "Interface\\TargetingFrame\\UI-StatusBar.blp",
    "Interface\\TargetingFrame\\UI-SmallTargetingFrame.blp",
    "Interface\\TargetingFrame\\UI-TargetofTargetFrame.blp",
    "Interface\\TargetingFrame\\TargetDead.blp",
    "Interface\\TargetingFrame\\UI-RaidTargetingIcons.blp",
    "Interface\\TargetingFrame\\UI-PVP-Alliance.blp",
    "Interface\\TargetingFrame\\UI-PVP-Horde.blp",
    // Parity (unit frames): the free-for-all badge, the player frame's combat backdrop
    "Interface\\TargetingFrame\\UI-PVP-FFA.blp",
    "Interface\\TargetingFrame\\UI-TargetingFrame-AttackBackground.blp",
    "Interface\\CharacterFrame\\UI-Player-Status.blp",
    "Interface\\CharacterFrame\\UI-StateIcon.blp",
    "Interface\\CharacterFrame\\TempPortrait*",
    "Interface\\CharacterFrame\\TemporaryPortrait*",
    "Interface\\CharacterFrame\\UI-BarFill-Simple.blp",
    // Casting bar, action buttons, auras
    "Interface\\CastingBar\\*",
    "Interface\\Buttons\\UI-Quickslot2.blp",
    "Interface\\Buttons\\UI-Quickslot.blp", // parity: an empty action button
    "Interface\\Buttons\\UI-Quickslot-Depress.blp",
    "Interface\\Buttons\\UI-QuickslotRed.blp",
    "Interface\\Buttons\\ButtonHilight-Square.blp",
    "Interface\\Buttons\\CheckButtonHilight.blp",
    "Interface\\Buttons\\UI-ActionButton-Border.blp",
    "Interface\\Buttons\\UI-EmptySlot.blp",
    "Interface\\Buttons\\UI-EmptySlot-White.blp",
    "Interface\\Buttons\\UI-Debuff-Border.blp",
    "Interface\\Buttons\\UI-Debuff-Overlays.blp",
    "Interface\\Buttons\\UI-TempEnchant-Border.blp",
    "Interface\\Buttons\\IconBorder-GlowRing.blp",
    "Interface\\Buttons\\UI-AutoCastableOverlay.blp",
    "Interface\\Buttons\\WHITE8X8.blp",
    // Panels, buttons and controls
    "Interface\\Buttons\\UI-Panel-Button-*",
    "Interface\\Buttons\\UI-Panel-MinimizeButton-*",
    "Interface\\Buttons\\UI-DialogBox-Button-*",
    "Interface\\Buttons\\UI-CheckBox-*",
    "Interface\\Buttons\\UI-SliderBar-*",
    "Interface\\Buttons\\UI-ScrollBar-*",
    "Interface\\Buttons\\UI-PlusButton-*",
    "Interface\\Buttons\\UI-MinusButton-*",
    "Interface\\Buttons\\UI-Listbox-Highlight*",
    "Interface\\Buttons\\UI-SpellbookIcon-NextPage-*",
    "Interface\\Buttons\\UI-SpellbookIcon-PrevPage-*",
    "Interface\\DialogFrame\\*",
    "Interface\\Tooltips\\UI-Tooltip-Background.blp",
    "Interface\\Tooltips\\UI-Tooltip-Border.blp",
    "Interface\\Tooltips\\UI-StatusBar-Border.blp",
    "Interface\\Tooltips\\ChatBubble-*",
    "Interface\\Common\\Common-Input-Border.blp",
    // Chat
    "Interface\\ChatFrame\\ChatFrameBackground.blp",
    "Interface\\ChatFrame\\UI-ChatInputBorder*",
    "Interface\\ChatFrame\\UI-ChatIcon-Scroll*",
    "Interface\\ChatFrame\\UI-ChatIcon-Chat-*",
    "Interface\\ChatFrame\\ChatFrameExpandArrow.blp", // phase 13: UIDropDownMenu's nested buttons
    // NPCs, quests, loot, money
    "Interface\\GossipFrame\\*",
    "Interface\\QuestFrame\\UI-Quest-BulletPoint.blp",
    "Interface\\QuestFrame\\UI-QuestItemNameFrame.blp",
    "Interface\\QuestFrame\\UI-QuestItemHighlight.blp",
    "Interface\\QuestFrame\\UI-HorizontalBreak.blp",
    "Interface\\QuestFrame\\UI-QuestTitleHighlight.blp",
    "Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp",
    "Interface\\QuestFrame\\UI-QuestLog-BookIcon.blp",
    "Interface\\QuestFrame\\UI-QuestLogSortTab-*", // phase 13: the quest log's "All" tab and Track Quest's radio
    "Interface\\Buttons\\UI-RadioButton.blp",
    "Interface\\QuestFrame\\UI-QuestGreeting-*",
    "Interface\\QuestFrame\\UI-TextBackground-*",
    "Interface\\MoneyFrame\\UI-MoneyIcons.blp",
    "Interface\\LootFrame\\LootPanel-Icon.blp",
    "Interface\\LootFrame\\UI-LootPanel.blp", // parity: LootFrame
    // Phase 15 (items): ContainerFrame, the keyring, ShoppingTooltip, DressUpFrame, master loot
    // Minimap and experience bar
    "Interface\\Minimap\\UI-Minimap-Border.blp",
    "Interface\\Minimap\\MinimapArrow.blp",
    "Interface\\Minimap\\ROTATING-MINIMAPCORPSEARROW.blp",
    "Interface\\Minimap\\ROTATING-MINIMAPGUIDEARROW.blp", // phase 13: the gossip point beyond the map
    "Interface\\Minimap\\ObjectIcons.blp",
    "Interface\\Minimap\\POIICONS.BLP",
    "Interface\\Minimap\\MiniMap-TrackingBorder.blp",
    "Interface\\Minimap\\UI-Minimap-ZoomInButton-*",
    "Interface\\Minimap\\UI-Minimap-ZoomOutButton-*",
    // Minimap cluster (parity pass): the zoom buttons' highlight, the day / night dial, the party's rim arrow, the ping
    "Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight.blp",
    "Interface\\Minimap\\UI-TOD-Indicator.blp",
    "Interface\\Minimap\\Rotating-MinimapGroupArrow.blp",
    "Interface\\Minimap\\Ping\\ping2.blp",
    "Interface\\Minimap\\Ping\\ping4.blp",
    "Interface\\Minimap\\Ping\\ping5.blp",
    "Interface\\MainMenuBar\\UI-ExhaustionTickNormal.blp",
    // Parity (experience bar): the rest tick's highlight, the bar's segmented frame (MainMenuXPBarTexture0-3)
    "Interface\\MainMenuBar\\UI-ExhaustionTickHighlight.blp",
    "Interface\\MainMenuBar\\UI-MainMenuBar-Dwarf.blp",
    // Login and character screens
    "Interface\\Glues\\Common\\Glue-Panel-Button-*",
    "Interface\\Glues\\Common\\Glue-Tooltip-*",
    "Interface\\Glues\\Common\\Glues-WoW-Logo.blp",
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes.blp",
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Races.blp",
    // Phase 8 (abilities): the talent trees' backgrounds, combo points, a hunter pet's happiness.
    "Interface\\TalentFrame\\Druid*",
    "Interface\\TalentFrame\\Hunter*",
    "Interface\\TalentFrame\\Mage*",
    "Interface\\TalentFrame\\Paladin*",
    "Interface\\TalentFrame\\Priest*",
    "Interface\\TalentFrame\\Rogue*",
    "Interface\\TalentFrame\\Shaman*",
    "Interface\\TalentFrame\\Warlock*",
    "Interface\\TalentFrame\\Warrior*",
    "Interface\\ComboFrame\\ComboPoint.blp",
    "Interface\\PetPaperDollFrame\\UI-PetHappiness.blp",
    // Phase 15 (pets): the pet's spellbook and paper doll, PartyMemberPetFrame, StableFrame
    "Interface\\PaperDollInfoFrame\\UI-Character-ResistanceIcons.blp", // PetMagicResFrame1..5
    "Interface\\CharacterFrame\\UI-CharacterFrame-GroupIndicator.blp", // PlayerFrameGroupIndicator
    // Social (phase 8): character creation, party frames, chat tabs, the social window's tabs
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Gender.blp",
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Factions.blp",
    "Interface\\Glues\\CharacterCreate\\UI-RotationRight-Big-*",
    "Interface\\Glues\\Common\\Glue-LeftArrow-Button-*",
    "Interface\\Glues\\Common\\Glue-RightArrow-Button-*",
    "Interface\\TargetingFrame\\UI-PartyFrame.blp",
    "Interface\\GroupFrame\\UI-Group-LeaderIcon.blp",
    "Interface\\GroupFrame\\UI-Group-MasterLooter.blp",
    "Interface\\ChatFrame\\ChatFrameTab.blp",
    "Interface\\FriendsFrame\\UI-FriendsFrameTab.blp",
    "Interface\\FriendsFrame\\UI-FriendsFrameTab-InactiveTab.blp",
    "Interface\\FriendsFrame\\UI-FriendsFrame-HighlightBar.blp", // retouches (friends frame): the guild's rows
    // Phase 15 (guild): GuildInfoFrame, GuildControlPopupFrame, GuildRegistrarFrame, PetitionFrame, TabardFrame,
    // HelpFrame
    "Interface\\FriendsFrame\\UI-GuildMember-Patch.blp",
    "Interface\\FriendsFrame\\WhoFrame-ColumnTabs.blp",
    "Interface\\Glues\\CharacterCreate\\CharacterCreate-LabelFrame.blp",
    "Interface\\PetitionFrame\\GuildCharter-Icon.blp",
    "Interface\\TabardFrame\\TabardFrameBackground.blp",
    "Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-*",
    "Interface\\MainMenuBar\\UI-MainMenu-ScrollUpButton-*",
    "Interface\\HelpFrame\\OpenTicketIcon.blp",
    "Interface\\HelpFrame\\HelpFrameButton-Highlight.blp",
    // Items (phase 8): what goes in each empty equipment and bag slot, the durability figure
    "Interface\\PaperDoll\\UI-PaperDoll-Slot-*",
    "Interface\\PaperDoll\\UI-Backpack-EmptySlot.blp",
    "Interface\\Durability\\UI-Durability-Icons.blp",
    // Phase 15 (character): PaperDollFrame, HonorFrame, ReputationFrame, SkillFrame, InspectFrame
    "Interface\\PaperDollInfoFrame\\UI-Character-StatBackground.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-ResistanceIcons.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Skills-Bar.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Skills-BarBorder.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Skills-BarBorderHighlight.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-ReputationBar.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-ReputationBar-Highlight.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Reputation-DetailBackground.blp",
    "Interface\\PaperDollInfoFrame\\UI-ReputationWatchBar.blp",
    "Interface\\Buttons\\CancelButton-*",
    "Interface\\PvPRankBadges\\PvPRank*", // the honor tabs' rank badges (32 x 32: no icon slot takes them)
    // Group loot rolls (phase 8): need, greed and pass (GroupLootFrame)
    "Interface\\Buttons\\UI-GroupLoot-Dice-Up.blp",
    "Interface\\Buttons\\UI-GroupLoot-Dice-Down.blp",
    "Interface\\Buttons\\UI-GroupLoot-Coin-Up.blp",
    "Interface\\Buttons\\UI-GroupLoot-Coin-Down.blp",
    "Interface\\Buttons\\UI-GroupLoot-Pass-Up.blp",
    "Interface\\Buttons\\UI-GroupLoot-Pass-Down.blp",
    // World feedback (parity): the name plates (NamePlateFrame) and the client's cursors over units and objects,
    // shown on the context button
    "Interface\\Tooltips\\Nameplate-Border.blp",
    "Interface\\Cursor\\Attack.blp",
    "Interface\\Cursor\\Pickup.blp",
    "Interface\\Cursor\\Speak.blp",
    "Interface\\Cursor\\Buy.blp",
    "Interface\\Cursor\\Repair.blp",
    "Interface\\Cursor\\Trainer.blp",
    "Interface\\Cursor\\Taxi.blp",
    "Interface\\Cursor\\Mail.blp",
    "Interface\\Cursor\\Interact.blp",
    "Interface\\Cursor\\PickLock.blp",
    "Interface\\Cursor\\GatherHerbs.blp",
    "Interface\\Cursor\\Mine.blp",
    // Parity (chat): the bottom button's flash, the docked tabs' flash and highlight, the chat buttons' highlight
    "Interface\\ChatFrame\\UI-ChatIcon-BlinkHilight.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp",
    "Interface\\Buttons\\UI-Common-MouseHilight.blp",
    // Parity pass (micro menu): MainMenuBarMicroButtons' art (the character button's own Up / Down), the keyring
    "Interface\\Buttons\\UI-MicroButton-*",
    "Interface\\Buttons\\UI-MicroButtonCharacter-*",
    "Interface\\Buttons\\UI-Button-KeyRing*",
    // Phase 15 (options): MacroFrame, UIOptionsFrame, SoundOptionsFrame
    "Interface\\Buttons\\UI-EmptySlot-Disabled.blp", // MacroFrameButtonTemplate's background
    "Interface\\CharacterFrame\\UI-Party-Background.blp", // PartyMemberBackground (SHOW_PARTY_BACKGROUND)
    "Interface\\CharacterFrame\\UI-Party-Border.blp",
    "Interface\\OptionsFrame\\OptionsFrameTab-Active.blp", // UIOptionsFrameTab1 / 2
    "Interface\\HelpFrame\\HelpFrameTab-Inactive.blp",
    "Interface\\HelpFrame\\HelpFrameTab-Active.blp", // MacroFrameTab1 / 2 (TabButtonTemplate)
    // Phase 13 (combat): LowHealthFrame and OutOfControlFrame (UIParent.xml), the full-screen status flashes
    "Interface\\FullScreenTextures\\LowHealth.blp",
    "Interface\\FullScreenTextures\\OutOfControl.blp",
    // Phase 15 (lead): model frames, the world map
    "Interface\\Buttons\\UI-RotationLeft-Button-*",
    "Interface\\Buttons\\UI-RotationRight-Button-*",
    "Interface\\Buttons\\ButtonHilight-Round.blp",
    // Phase 16 (raidframe): RaidFrame, RaidGroupFrame (Blizzard_RaidUI)
    "Interface\\RaidFrame\\UI-RaidFrame-GroupOutline.blp",
    "Interface\\RaidFrame\\UI-RaidFrame-GroupButton.blp", // with alpha: no ADD baking needed
    // Phase 16 (pullouts): RaidPulloutFrame, RaidPulloutButton
    "Interface\\RaidFrame\\UI-RaidFrame-HealthBar.blp",
    // Phase 16 (alerts): ReadyCheckFrame, MiniMapMeetingStoneFrame
    "Interface\\RaidFrame\\UI-ReadyCheckFrame.blp", // its art (the meeting stone's button uses what the minimap has)
    // Phase 17 (queue): BattlefieldFrame, MiniMapBattlefieldFrame
    "Interface\\BattlefieldFrame\\UI-Battlefield-Icon.blp",    // BattlefieldFramePortrait
    "Interface\\BattlefieldFrame\\Battleground-Alliance.blp", // MiniMapBattlefieldIcon
    "Interface\\BattlefieldFrame\\Battleground-Horde.blp",
    // Phase 17 (scores): WorldStateAlwaysUpFrame, WorldStateCaptureBar, WorldStateScoreFrame, BattlefieldMinimap
    "Interface\\WorldStateFrame\\AllianceFlag.blp", // the lines' dynamic icons, the battle map's flags
    "Interface\\WorldStateFrame\\HordeFlag.blp",
    "Interface\\WorldStateFrame\\AllianceTower.blp", // the Eastern Plaguelands' lines
    "Interface\\WorldStateFrame\\HordeTower.blp",
    "Interface\\WorldStateFrame\\WorldState-CaptureBar.blp",
    "Interface\\WorldStateFrame\\WorldStateFinalScoreFrame-*", // the score frame's art and its top background
    "Interface\\WorldStateFrame\\WorldStateFinalScore-Highlight.blp",
    "Interface\\WorldStateFrame\\ColumnIcon-*", // the score columns' icons (0 Horde, 1 Alliance)
    "Interface\\PaperDollInfoFrame\\UI-Character-ActiveTab.blp", // its tabs
    "Interface\\PaperDollInfoFrame\\UI-Character-InActiveTab.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-ScrollBar.blp", // its scroll bar
    "Interface\\BattlefieldFrame\\UI-BattlefieldMinimap-Border.blp",
    "Interface\\WorldStateFrame\\AllianceFlagFlash.blp", // ADD (kAdditiveTextures)
    "Interface\\WorldStateFrame\\HordeFlagFlash.blp",
    // Phase 17 (honor): the area spirit healer, PvP
    // Phase 18 (glue):
    "Interface\\Glues\\MainMenu\\Glues-BlizzardLogo.blp", // AccountLogin
    "Interface\\Glues\\Login\\Glues-ESRBRating.blp",      // LocalizeFrames (enUS)
    "Interface\\Glues\\CharacterSelect\\Glue-CharacterSelect-Highlight.blp", // no alpha: drawn additively
    "Interface\\HelpFrame\\HelpFrame-TopLeft.blp", // RealmListBackground
    "Interface\\HelpFrame\\HelpFrame-Top.blp",
    "Interface\\HelpFrame\\HelpFrame-TopRight.blp",
    "Interface\\HelpFrame\\HelpFrame-BotLeft.blp",
    "Interface\\HelpFrame\\HelpFrame-Bottom.blp",
    "Interface\\HelpFrame\\HelpFrame-BotRight.blp",
    "Interface\\Buttons\\UI-SortArrow.blp", // RealmSortButtonTemplate
    // Phase 18 (create):
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-OuterBorder.blp", // the configuration frame's border
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Background.blp",
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Banners.blp", // behind the race buttons
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-IconShadow.blp", // under the class and gender buttons
    "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-ScrollBar-Top.blp", // the texts' scroll troughs
    "Interface\\ClassTrainerFrame\\UI-ClassTrainer-ScrollBar.blp",
    "Interface\\Glues\\Common\\TextPanel-Border.blp", // the faction, race and class texts' border
    // Phase 18 (p3):
    "Interface\\TutorialFrame\\TutorialFrameAlert.blp", // TutorialFrameAlertButton (its highlight: the right half)
    "Interface\\MainMenuBar\\UI-MainMenuBar-PerformanceBar.blp", // MainMenuBarPerformanceBar (the latency)
    "Interface\\Buttons\\UI-SortArrow.blp", // AuctionSortButtonTemplate's arrow (the sortable columns)
    // Phase 18 (extras):
    "Interface\\Buttons\\CheckButtonGlow.blp", // SpellBookSkillLineTab<n>Flash (ADD, with its alpha: additive)
    // Retouches (ui models):
    "Interface\\Buttons\\GlowStar.blp", // UI-AutoCastButton.m2's particles (blended additively, with their alpha)
    "Interface\\Cooldown\\star4.blp", // UI-Cooldown-Indicator.m2's flash at the end (ADD: kAdditiveTextures)
    // Retouches (windows):
    "Interface\\TalentFrame\\UI-TalentBranches.blp",     // TalentBranchTemplate (TALENT_BRANCH_TEXTURECOORDS)
    "Interface\\TalentFrame\\UI-TalentArrows.blp",       // TalentArrowTemplate (TALENT_ARROW_TEXTURECOORDS)
    "Interface\\TalentFrame\\TalentFrame-RankBorder.blp", // TalentButtonTemplate's RankBorder
    "Interface\\SpellBook\\SpellBook-SkillLineTab.blp",   // SpellBookSkillLineTabTemplate
    "Interface\\SpellBook\\UI-SpellBook-Tab-Unselected.blp", // SpellBookFrameTabButtonTemplate
    "Interface\\SpellBook\\UI-SpellBook-Tab1-Selected.blp",
    "Interface\\SpellBook\\UI-SpellbookPanel-Tab-Highlight.blp", // its highlight (ADD, with its alpha)
    "Interface\\TaxiFrame\\UI-Taxi-Line.blp",             // DrawRouteLine (patch.MPQ)
    "Interface\\TradeFrame\\UI-TradeFrame-Highlight.blp", // TradeHighlightTemplate
    "Interface\\MailFrame\\UI-MailFrame-InvoiceLine.blp", // OpenMailArithmeticLine
    "Interface\\Icons\\INV_Misc_QuestionMark.blp",        // an icon still loading (or unknown)
    // Retouches (world):
    "Interface\\Tooltips\\Nameplate-Glow.blp", // the plate under the mouse in 1.12, the target's here (ADD, no alpha)
};

// Highlights the client blends additively (alphaMode="ADD" in FrameXML) and ships without alpha: black stands for
// "nothing". The UI blends by alpha, so they are baked with their brightness as alpha (and the color divided by it):
// over the interface they add their glow like the client does, instead of covering it with black.
// (Phase 22: our own art, phase 20's kOwnArt, is baked into app.vdpk: tools/formats/src/bake/AppPackBaker.cpp.)
constexpr const char* kAdditiveTextures[] = {
    "Interface\\Buttons\\ButtonHilight-Square.blp",
    "Interface\\Buttons\\CheckButtonHilight.blp",
    "Interface\\Buttons\\UI-Panel-Button-Highlight.blp",
    "Interface\\Buttons\\UI-Panel-Button-Highlight2.blp",
    "Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight.blp",
    "Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight.blp",
    "Interface\\Minimap\\Ping\\ping2.blp",
    "Interface\\Minimap\\Ping\\ping4.blp",
    "Interface\\Minimap\\Ping\\ping5.blp",
    "Interface\\Buttons\\UI-DialogBox-Button-Highlight.blp",
    "Interface\\Buttons\\UI-CheckBox-Highlight.blp",
    "Interface\\Buttons\\UI-PlusButton-Hilight.blp",
    "Interface\\Buttons\\UI-Listbox-Highlight.blp",
    "Interface\\Buttons\\UI-Listbox-Highlight2.blp",
    "Interface\\Buttons\\UI-ScrollBar-ScrollUpButton-Highlight.blp",
    "Interface\\Buttons\\UI-ScrollBar-ScrollDownButton-Highlight.blp",
    "Interface\\QuestFrame\\UI-QuestTitleHighlight.blp",
    "Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp",
    "Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp", // parity: the chat
    "Interface\\Buttons\\UI-Common-MouseHilight.blp",
    "Interface\\Buttons\\UI-Button-KeyRing-Highlight.blp", // parity pass: the micro menu's bag bar
    "Interface\\FullScreenTextures\\LowHealth.blp",         // phase 13 (combat)
    "Interface\\FullScreenTextures\\OutOfControl.blp",
    "Interface\\Buttons\\ButtonHilight-Round.blp", // phase 15 (lead): the model frames' rotate buttons
    "Interface\\PaperDollInfoFrame\\UI-Character-ReputationBar-Highlight.blp", // phase 15 (character)
    "Interface\\HelpFrame\\HelpFrameButton-Highlight.blp", // phase 15 (guild): HelpFrame's buttons
    // Phase 17 (scores).
    "Interface\\WorldStateFrame\\AllianceFlagFlash.blp", // the lines' flashing flags (no alpha: ADD)
    "Interface\\WorldStateFrame\\HordeFlagFlash.blp",
    // Phase 18 (glue):
    // Phase 18 (create):
    // Phase 18 (p3):
    // Retouches (ui models):
    "Interface\\Cooldown\\star4.blp", // the cooldown's flash (no alpha: the model blends it additively)
    // Retouches (world):
    "Interface\\Tooltips\\Nameplate-Glow.blp",
};

// An additive texture without alpha as one blended by alpha: alpha = the brightest channel, color / alpha.
void additiveToAlpha(formats::BlpImage& image) {
    for (std::vector<u8>& mip : image.mips) {
        for (usize i = 0; i + 3 < mip.size(); i += 4) {
            const u32 a = std::max({mip[i], mip[i + 1], mip[i + 2]});
            for (usize c = 0; c < 3; ++c) {
                mip[i + c] = a > 0 ? static_cast<u8>(std::min<u32>(255, (mip[i + c] * 255u + a / 2) / a)) : 0;
            }
            mip[i + 3] = static_cast<u8>(a);
        }
    }
    image.alphaDepth = 8;
}

// The TrueType fonts of the client, as "ui/font/<name>".
struct FontSpec {
    const char* path;
    const char* name;
    f32 pixelHeight;
};
constexpr FontSpec kFonts[] = {
    {"Fonts\\FRIZQT__.TTF", "frizqt", 40.0f},   // everything by default
    {"Fonts\\ARIALN.TTF", "arialn", 40.0f},     // chat, numbers
    {"Fonts\\MORPHEUS.TTF", "morpheus", 40.0f}, // quest and book text
    {"Fonts\\SKURRI.TTF", "skurri", 40.0f},     // floating combat numbers
};

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// The files of a kUiTextures entry.
std::vector<std::string> expand(const formats::MpqArchiveSet& mpq, std::string_view entry) {
    if (entry.back() != '*') {
        return {std::string(entry)};
    }
    const usize slash = entry.rfind('\\');
    const std::string directory(entry.substr(0, slash + 1));
    const std::string prefix = lower(std::string(entry.substr(slash + 1, entry.size() - slash - 2)));
    std::vector<std::string> files;
    for (const std::string& file : mpq.list((directory + "*").c_str())) {
        const std::string name = lower(file.substr(file.rfind('\\') + 1));
        if (name.rfind(prefix, 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".blp") == 0 &&
            file.size() > directory.size() && file.find('\\', directory.size()) == std::string::npos) {
            files.push_back(file);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// textures\Minimap\md5translate.trs: "dir: <map>" sections of "<map>\map<x>_<y>.blp<TAB><md5>.blp" lines.
struct MinimapTile {
    std::string map;
    u32 x;
    u32 y;
    std::string texture;
};
std::vector<MinimapTile> parseMinimapTable(std::string_view text) {
    std::vector<MinimapTile> tiles;
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        const usize tab = line.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }
        const std::string_view name = line.substr(0, tab);
        const std::string_view hashed = line.substr(tab + 1);
        const usize slash = name.find('\\');
        if (slash == std::string_view::npos || name.find('\\', slash + 1) != std::string_view::npos) {
            continue; // WMO interiors: "World\wmo\..."
        }
        u32 x = 0, y = 0;
        const std::string file(name.substr(slash + 1));
        if (std::sscanf(file.c_str(), "map%u_%u.blp", &x, &y) != 2 || x > 63 || y > 63) {
            continue;
        }
        tiles.push_back({std::string(name.substr(0, slash)), x, y, "textures\\Minimap\\" + std::string(hashed)});
    }
    return tiles;
}

// Phase 15: the "WMO\<dir>\<name>_<group>_<x>_<y>.blp" lines of the same table: the indoor minimap tiles of every
// building, keyed by its WMO root ("World\wmo\<dir>\<name>.wmo").
struct WmoMinimapTile {
    std::string wmo;
    u32 group;
    u32 x;
    u32 y;
    std::string texture;
};
std::vector<WmoMinimapTile> parseWmoMinimapTable(std::string_view text) {
    std::vector<WmoMinimapTile> tiles;
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        const usize tab = line.find('\t');
        if (tab == std::string_view::npos || line.size() < 4 || lower(std::string(line.substr(0, 4))) != "wmo\\") {
            continue;
        }
        const std::string name(line.substr(0, tab));
        const std::string_view hashed = line.substr(tab + 1);
        // <path>_<ggg>_<xx>_<yy>.blp
        const usize dot = name.rfind('.');
        if (dot == std::string::npos || dot < 12) {
            continue;
        }
        u32 group = 0, x = 0, y = 0;
        if (std::sscanf(name.c_str() + dot - 10, "_%3u_%2u_%2u", &group, &x, &y) != 3) {
            continue;
        }
        tiles.push_back({"World\\wmo" + name.substr(3, dot - 10 - 3) + ".wmo", group, x, y,
                         "textures\\Minimap\\" + std::string(hashed)});
    }
    return tiles;
}

// A BLP2 file's size (its header: "BLP2", type, 4 bytes of encoding, width, height).
bool blpSize(std::span<const u8> blp, u32& width, u32& height) {
    if (blp.size() < 20 || std::memcmp(blp.data(), "BLP2", 4) != 0) {
        return false;
    }
    std::memcpy(&width, blp.data() + 12, 4);
    std::memcpy(&height, blp.data() + 16, 4);
    return width > 0 && height > 0 && width <= 4096 && height <= 4096;
}

} // namespace

// Phase 15 (options): the macro icons' list ("ui/macroicons", tools/baker/MacroIcons.cpp).
std::vector<u8> bakeMacroIcons(const formats::MpqArchiveSet& mpq, usize& count);
// Retouches (ui models): the interface's models ("ui/models", tools/baker/UiModels.cpp); their textures in `sprites`.
std::vector<u8> bakeUiModels(const formats::MpqArchiveSet& mpq, std::span<const pack::UiSprite> sprites, usize& count);

// The interface's texts of the clients (phase 22: ours are in app.vdpk): the base client's table under its language
// (the pack's header says which: core/Text falls back to it) and, phase 21, one per client of another language with
// what that client defines (ADR-0004), each with the rules derived from its Lua.
bool bakeStrings(const formats::MpqArchiveSet& mpq, const std::string& baseLocale,
                 const std::vector<LocaleClient>& localeClients,
                 std::vector<std::pair<std::string, std::vector<u8>>>& out, usize& clientCount) {
    std::vector<bake::ClientTexts> clients{{baseLocale, &mpq}};
    for (const LocaleClient& client : localeClients) {
        clients.push_back({client.locale, client.mpq.get()});
    }
    bake::TextTables tables;
    bool ok = bake::clientTextTables(clients, tables);
    clientCount = tables.empty() ? 0 : tables.front().second.size();
    for (const auto& [locale, texts] : tables) {
        VD_LOG_INFO("texts %s: %zu of its client", locale.c_str(), texts.size());
    }
    ok &= bake::stringTableBlobs(tables, out);
    return ok;
}

int bakeUi(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath, const std::string& baseLocale,
           const std::vector<LocaleClient>& localeClients) {
    const Clock::time_point start = Clock::now();
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    writer.setLocale(baseLocale); // phase 22: the base client's texts are under this language
    int failures = 0;

    // Sprites.
    std::vector<std::string> paths;
    for (const char* entry : kUiTextures) {
        std::vector<std::string> files = expand(mpq, entry);
        if (files.empty()) {
            VD_LOG_ERROR("UI texture not found: %s", entry);
            ++failures;
        }
        for (std::string& file : files) {
            if (std::find(paths.begin(), paths.end(), file) == paths.end()) {
                paths.push_back(std::move(file));
            }
        }
    }
    std::vector<formats::BlpImage> images;
    images.reserve(paths.size()); // the inputs point into it: never reallocated
    std::vector<bake::UiSpriteInput> inputs;
    for (const std::string& path : paths) {
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::BlpImage> image = bytes ? formats::decodeBlp(bytes.value()) : Result<formats::BlpImage>(bytes.error());
        if (!image) {
            VD_LOG_ERROR("UI texture %s: %s", path.c_str(), image.error().message.c_str());
            ++failures;
            continue;
        }
        const bool additive = std::any_of(std::begin(kAdditiveTextures), std::end(kAdditiveTextures),
                                          [&](const char* entry) { return lower(entry) == lower(path); });
        if (additive && image.value().alphaDepth == 0) {
            additiveToAlpha(image.value());
        }
        images.push_back(std::move(image.value()));
        inputs.push_back({path, &images.back()});
    }
    Result<bake::UiAtlas> atlas = bake::packUiAtlas(inputs);
    if (!atlas) {
        VD_LOG_ERROR("%s", atlas.error().message.c_str());
        return 1;
    }
    std::vector<std::vector<u8>> pageBlobs;
    for (std::vector<u8>& page : atlas.value().pages) {
        formats::BlpImage image;
        image.width = atlas.value().pageSize;
        image.height = atlas.value().pageSize;
        image.alphaDepth = 8;
        image.encoding = formats::BlpEncoding::Argb8888;
        image.mips.push_back(std::move(page));
        Result<std::vector<u8>> blob = bake::bakeTexture(image, pack::TextureFormat::Astc4x4);
        if (!blob) {
            VD_LOG_ERROR("UI atlas page: %s", blob.error().message.c_str());
            return 1;
        }
        pageBlobs.push_back(std::move(blob.value()));
    }
    Result<std::vector<u8>> atlasBlob = bake::buildTextureArray(pageBlobs);
    if (!atlasBlob) {
        VD_LOG_ERROR("UI atlas: %s", atlasBlob.error().message.c_str());
        return 1;
    }
    const std::vector<pack::UiSprite>& sprites = atlas.value().sprites;
    std::vector<u8> spriteTable(sizeof(pack::UiSpritesHeader) + sprites.size() * sizeof(pack::UiSprite));
    const pack::UiSpritesHeader spritesHeader{static_cast<u32>(sprites.size()), 0};
    std::memcpy(spriteTable.data(), &spritesHeader, sizeof(spritesHeader));
    std::memcpy(spriteTable.data() + sizeof(spritesHeader), sprites.data(), sprites.size() * sizeof(pack::UiSprite));

    // Fonts.
    std::vector<std::vector<u8>> ttfs;
    std::vector<bake::UiFontInput> fontInputs;
    for (const FontSpec& spec : kFonts) {
        Result<std::vector<u8>> bytes = mpq.read(spec.path);
        if (!bytes) {
            VD_LOG_ERROR("Font %s: %s", spec.path, bytes.error().message.c_str());
            ++failures;
            continue;
        }
        ttfs.push_back(std::move(bytes.value()));
    }
    for (usize i = 0; i < ttfs.size(); ++i) {
        bake::UiFontInput input;
        input.name = kFonts[i].name;
        input.ttf = ttfs[i];
        input.pixelHeight = kFonts[i].pixelHeight;
        fontInputs.push_back(input);
    }
    const std::vector<u32> charset = bake::uiCharacterSet();
    Result<bake::UiFontBake> fonts = bake::bakeUiFonts(fontInputs, charset);
    if (!fonts) {
        VD_LOG_ERROR("%s", fonts.error().message.c_str());
        return 1;
    }

    // Minimap tiles of every map.
    std::vector<std::pair<std::string, std::vector<u8>>> minimaps;
    usize minimapTiles = 0;
    if (Result<std::vector<u8>> table = mpq.read("textures\\Minimap\\md5translate.trs")) {
        std::vector<MinimapTile> tiles = parseMinimapTable(
            std::string_view(reinterpret_cast<const char*>(table.value().data()), table.value().size()));
        std::sort(tiles.begin(), tiles.end(), [](const MinimapTile& a, const MinimapTile& b) {
            return a.map != b.map ? lower(a.map) < lower(b.map) : (a.y << 8 | a.x) < (b.y << 8 | b.x);
        });
        for (usize i = 0; i < tiles.size();) {
            usize j = i;
            std::vector<pack::UiMinimapTile> entries;
            while (j < tiles.size() && lower(tiles[j].map) == lower(tiles[i].map)) {
                pack::UiMinimapTile tile{};
                tile.x = static_cast<u8>(tiles[j].x);
                tile.y = static_cast<u8>(tiles[j].y);
                tile.textureHash = assetPathHash(tiles[j].texture);
                if (entries.empty() || (entries.back().x != tile.x || entries.back().y != tile.y)) {
                    entries.push_back(tile);
                }
                ++j;
            }
            std::vector<u8> blob(sizeof(pack::UiMinimapHeader) + entries.size() * sizeof(pack::UiMinimapTile));
            const pack::UiMinimapHeader header{static_cast<u32>(entries.size()), 0};
            std::memcpy(blob.data(), &header, sizeof(header));
            std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiMinimapTile));
            minimaps.emplace_back("ui/minimap/" + lower(tiles[i].map), std::move(blob));
            minimapTiles += entries.size();
            i = j;
        }
    } else {
        VD_LOG_ERROR("textures\\Minimap\\md5translate.trs: %s", table.error().message.c_str());
        ++failures;
    }

    // Phase 15: the indoor minimaps (pack/UiFormat.h: kUiWmoMinimapsAsset), placed by the groups' boxes of each WMO.
    std::vector<u8> wmoMinimaps;
    usize wmoMinimapCount = 0, wmoMinimapTiles = 0;
    if (Result<std::vector<u8>> table = mpq.read("textures\\Minimap\\md5translate.trs")) {
        std::vector<WmoMinimapTile> tiles = parseWmoMinimapTable(
            std::string_view(reinterpret_cast<const char*>(table.value().data()), table.value().size()));
        std::stable_sort(tiles.begin(), tiles.end(), [](const WmoMinimapTile& a, const WmoMinimapTile& b) {
            return assetPathHash(a.wmo) < assetPathHash(b.wmo);
        });
        std::vector<pack::UiWmoMinimap> wmos;
        std::vector<pack::UiWmoMinimapTile> entries;
        usize skipped = 0;
        for (usize i = 0; i < tiles.size();) {
            usize j = i;
            const u64 wmoHash = assetPathHash(tiles[i].wmo);
            while (j < tiles.size() && assetPathHash(tiles[j].wmo) == wmoHash) {
                ++j;
            }
            Result<std::vector<u8>> rootBytes = mpq.read(tiles[i].wmo);
            Result<formats::WmoRoot> root =
                rootBytes ? formats::parseWmoRoot(rootBytes.value()) : Result<formats::WmoRoot>(rootBytes.error());
            if (!root) {
                skipped += j - i; // a test building the client lists but does not ship
                i = j;
                continue;
            }
            pack::UiWmoMinimap wmo{wmoHash, static_cast<u32>(entries.size()), 0};
            for (usize k = i; k < j; ++k) {
                const WmoMinimapTile& t = tiles[k];
                u32 width = 0, height = 0;
                Result<std::vector<u8>> blp = mpq.read(t.texture);
                if (t.group >= root.value().groups.size() || !blp || !blpSize(blp.value(), width, height)) {
                    ++skipped;
                    continue;
                }
                const formats::WmoGroupInfo& g = root.value().groups[t.group];
                pack::UiWmoMinimapTile tile{};
                tile.texture = assetPathHash(t.texture);
                tile.boundsMin[0] = g.boundsMin[0] + pack::kWmoMinimapTileYards * static_cast<f32>(t.x);
                tile.boundsMin[1] = g.boundsMin[1] + pack::kWmoMinimapTileYards * static_cast<f32>(t.y);
                tile.boundsMin[2] = g.boundsMin[2];
                tile.boundsMax[0] = tile.boundsMin[0] + static_cast<f32>(width) / pack::kWmoMinimapPixelsPerYard;
                tile.boundsMax[1] = tile.boundsMin[1] + static_cast<f32>(height) / pack::kWmoMinimapPixelsPerYard;
                tile.boundsMax[2] = g.boundsMax[2];
                entries.push_back(tile);
                ++wmo.tileCount;
            }
            if (wmo.tileCount > 0) {
                wmos.push_back(wmo);
            }
            i = j;
        }
        const pack::UiWmoMinimapHeader header{static_cast<u32>(wmos.size()), static_cast<u32>(entries.size())};
        wmoMinimaps.resize(sizeof(header) + wmos.size() * sizeof(pack::UiWmoMinimap) +
                           entries.size() * sizeof(pack::UiWmoMinimapTile));
        std::memcpy(wmoMinimaps.data(), &header, sizeof(header));
        std::memcpy(wmoMinimaps.data() + sizeof(header), wmos.data(), wmos.size() * sizeof(pack::UiWmoMinimap));
        std::memcpy(wmoMinimaps.data() + sizeof(header) + wmos.size() * sizeof(pack::UiWmoMinimap), entries.data(),
                    entries.size() * sizeof(pack::UiWmoMinimapTile));
        wmoMinimapCount = wmos.size();
        wmoMinimapTiles = entries.size();
        if (skipped > 0) {
            VD_LOG_INFO("ui: %zu indoor minimap tiles without their building or texture", skipped);
        }
    }

    auto add = [&](std::string_view path, pack::AssetType type, std::span<const u8> data, bool compress) {
        if (Result<void> added = writer.add(path, type, data, compress); !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            ++failures;
        }
    };
    add(pack::kUiAtlasAsset, pack::AssetType::Texture, atlasBlob.value(), false);
    add(pack::kUiSpritesAsset, pack::AssetType::UiSprites, spriteTable, false);
    add(pack::kUiFontAtlasAsset, pack::AssetType::Texture,
        bake::buildR8Array(fonts.value().layers, pack::kUiFontAtlasSize), true);
    for (const auto& [name, blob] : fonts.value().fonts) {
        add(name, pack::AssetType::UiFont, blob, false);
    }
    for (const auto& [name, blob] : minimaps) {
        add(name, pack::AssetType::UiMinimap, blob, false);
    }
    if (!wmoMinimaps.empty()) {
        add(pack::kUiWmoMinimapsAsset, pack::AssetType::UiMinimap, wmoMinimaps, false); // phase 15
    }
    std::vector<std::pair<std::string, std::vector<u8>>> texts;
    usize clientTexts = 0;
    if (!bakeStrings(mpq, baseLocale, localeClients, texts, clientTexts)) {
        ++failures;
    }
    for (const auto& [name, blob] : texts) {
        add(name, pack::AssetType::UiStrings, blob, false);
    }
    usize macroIcons = 0; // phase 15 (options)
    add(pack::kUiMacroIconsAsset, pack::AssetType::UiMacroIcons, bakeMacroIcons(mpq, macroIcons), false);
    VD_LOG_INFO("ui: %zu macro icons", macroIcons);
    usize uiModels = 0; // retouches (ui models)
    const std::vector<u8> uiModelsBlob = bakeUiModels(mpq, sprites, uiModels);
    if (uiModelsBlob.empty()) {
        ++failures;
    } else {
        add(pack::kUiModelsAsset, pack::AssetType::UiModels, uiModelsBlob, false);
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("ui: %zu textures in %zu atlas pages (%.0f%% full), %zu fonts (%zu characters, %zu layers), "
                "%zu minimaps (%zu tiles), %zu indoor minimaps (%zu tiles), texts in %zu languages (%zu of the client), "
                "%.1f MB, %d failed, %.1f s -> %s",
                sprites.size(), atlas.value().pages.size(), atlas.value().fill * 100.0f, fonts.value().fonts.size(),
                charset.size(), fonts.value().layers.size(), minimaps.size(), minimapTiles, wmoMinimapCount,
                wmoMinimapTiles, texts.size(), clientTexts,
                static_cast<double>(writer.bytesWritten()) / 1e6, failures, secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures == 0 ? 0 : 1;
}

} // namespace vd::baker
