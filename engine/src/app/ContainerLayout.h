#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>
#include <span>

namespace vd {

// Phase 15 (items): the bags as the original's ContainerFrames (ContainerFrame.lua / .xml,
// MainMenuBarBagButtons.lua), without drawing (unit tested): which frames are open and in what order
// (ContainerFrame1.bags), what the bag bar's buttons and OpenAllBags / CloseAllBags do to them, each frame's
// art and buttons for its size (ContainerFrame_GenerateFrame) and where the open frames stack
// (updateContainerFrameAnchors). Coordinates are the original's units (its frames are 192 wide) until the
// stacking turns them into logical pixels.

// ContainerFrame.lua's constants.
constexpr u32 kContainerFrames = 12;    // NUM_CONTAINER_FRAMES
constexpr u32 kContainerColumns = 4;    // NUM_CONTAINER_COLUMNS
constexpr f32 kContainerWidth = 192.0f; // CONTAINER_WIDTH
constexpr f32 kContainerSpacing = 0.0f; // CONTAINER_SPACING: between two frames of a column
constexpr f32 kVisibleContainerSpacing =
    3.0f;                             // VISIBLE_CONTAINER_SPACING: counted against the column's height
constexpr f32 kContainerScale = 0.9f; // CONTAINER_SCALE: the shrink step when the frames do not fit
constexpr f32 kContainerArtLeft =
    64.0f; // the 256-unit backgrounds hang this far left of the frame (portrait)
// The client's container numbers (ContainerFrame.lua, game::kContainer*): 0 the backpack, 1..4 the worn bags,
// 5..10 the bank's bags, -2 the keyring (KEYRING_CONTAINER).
constexpr i32 kKeyringContainer = -2;

// GetKeyRingSize (ContainerFrame.lua): the keyring shows 4 slots below level 40, 8 from 40, 12 from 50, 16
// above 60.
u32 keyringSize(u32 level);

// --- The open frames
// -------------------------------------------------------------------------------------------

// How many slots each container has now (0: no bag there), as GetContainerNumSlots answers.
struct ContainerSizes {
    std::array<u32, 5> bags{}; // the backpack, then bags 1..4
    u32 keyring = 0;           // keyringSize(level)
    u32 size(i32 id) const {
        return id == kKeyringContainer ? keyring : id >= 0 && id < 5 ? bags[static_cast<u32>(id)] : 0u;
    }
};

// The ContainerFrames shown, in the order they opened (ContainerFrame1.bags: the stacking order), and the
// functions of ContainerFrame.lua that open and close them.
class ContainerStack {
public:
    // ToggleBag(id): a bag with slots (or the keyring) opens, or closes when shown.
    void toggleBag(i32 id, const ContainerSizes& sizes);
    // ToggleBackpack: shown, every frame closes (the keyring too); else the backpack opens.
    void toggleBackpack(const ContainerSizes& sizes);
    // ToggleKeyRing.
    void toggleKeyring(const ContainerSizes& sizes) { toggleBag(kKeyringContainer, sizes); }
    // OpenAllBags(forceOpen): every frame closes; unless all the bags were open (the keyring not counted) and
    // not forced, the backpack and the bags with slots open again in order.
    void openAllBags(const ContainerSizes& sizes, bool force = false);
    // OpenBag(id) (BAG_OPEN): opens it unless shown. CloseBag(id) (BAG_CLOSED, its close button).
    void openBag(i32 id, const ContainerSizes& sizes);
    void closeBag(i32 id);
    // CloseAllBags: the backpack and the bags (CloseBag(1..12): the keyring stays).
    void closeAllBags();
    // CloseAllWindows: every ContainerFrame.
    void closeAll() { m_count = 0; }
    // An open frame becomes the newest (a frame kept out of sight for lack of room comes back: see
    // visibleContainers).
    void raise(i32 id);

    bool isOpen(i32 id) const;
    bool any() const { return m_count > 0; }
    std::span<const i32> order() const { return {m_order.data(), m_count}; }

private:
    void open(i32 id);

    std::array<i32, kContainerFrames> m_order{};
    u32 m_count = 0;
};

// --- One frame
// -------------------------------------------------------------------------------------------------

// Which background ContainerFrame_GenerateFrame gives a frame.
enum class ContainerArt : u8 {
    Backpack, // UI-BackpackBackground (with the money frame)
    Bag,      // UI-Bag-Components
    Bank,     // UI-Bag-Components-Bank (a bank bag)
    Keyring,  // UI-Bag-Components-Keyring
};
// A piece of the background: where it goes (units from the frame's top, 256 wide from kContainerArtLeft left
// of the frame) and the texture rows it shows.
struct ContainerPiece {
    f32 y = 0.0f, h = 0.0f;
    f32 v0 = 0.0f, v1 = 1.0f;
};
struct ContainerGeometry {
    ContainerArt art = ContainerArt::Bag;
    f32 height = 0.0f;                      // the frame's height (units); its width is kContainerWidth
    std::array<ContainerPiece, 4> pieces{}; // top, middle(s), bottom
    u32 pieceCount = 0;
    f32 firstItemBottom =
        9.0f; // the first item button's offset from the frame's bottom (30 for the backpack)
    u32 size = 0;
};
// ContainerFrame_GenerateFrame's background and height for container `id` with `size` slots (the backpack's
// 240-unit frame, else top + middles + bottom of UI-Bag-Components by rows of 4: the "+2" top for sizes with
// two left over, the one-row top for a single row).
ContainerGeometry containerGeometry(i32 id, u32 size);
// The item button of slot `slot` (0-based, the client's slot order: GetContainerItemInfo(id, slot + 1)), in
// units from the frame's top left: 37 x 37, in rows of four from the bottom right (the first row up top holds
// what is left over, right-aligned), 42 units across and 41 up apart.
ui::Rect containerItemRect(const ContainerGeometry& g, u32 slot);
// The other pieces of ContainerFrameTemplate, in units from the frame's top left.
constexpr ui::Rect kContainerPortrait{7.0f, 5.0f, 40.0f, 40.0f}; // $parentPortrait / PortraitButton
constexpr ui::Rect kContainerName{47.0f, 10.0f, 112.0f, 12.0f};  // $parentName (GameFontHighlight)
constexpr ui::Rect kContainerClose{160.0f, 1.0f, 32.0f, 32.0f};  // UIPanelCloseButton at TOPRIGHT (0, -1)
// The backpack's SmallMoneyFrame (TOPRIGHT -2, -216): its copper coin ends 13 units inside it (CopperButton at RIGHT
// -13, its icon at the button's RIGHT), so the coins end 177 units from the frame's left.
constexpr f32 kContainerMoneyRight = 177.0f;
constexpr f32 kContainerMoneyTop = 216.0f;

// --- Stacking
// --------------------------------------------------------------------------------------------------

// updateContainerFrameAnchors in `area` (logical pixels) at `scale` (logical pixels per unit): the first
// frame at the bottom right, each next one on top of the previous, a new column to the left once the next
// does not fit in what is left of the height. Writes each frame's rectangle; returns how many fit inside
// `area` (frames past that are placed too, further left).
u32 layoutContainers(std::span<const f32> heights, const ui::Rect& area, f32 scale, std::span<ui::Rect> out);
// The scale the frames are shown at: `largest` when they all fit, else shrunk by kContainerScale steps (the
// original's shrink pass) while some do not, down to `smallest`.
f32 containerScale(std::span<const f32> heights, const ui::Rect& area, f32 largest, f32 smallest);
// Touch: the frames never cover the movement stick or the action buttons, so when even at `smallest` they do
// not all fit in `area`, some stay open out of sight until there is room again (the bag bar brings one back):
// the newest always shows, and of the others the oldest that fit (the backpack first, at the bottom right).
// Writes the indexes (into `heights`) of the frames shown, oldest first; returns how many.
u32 shownContainers(std::span<const f32> heights, const ui::Rect& area, f32 smallest, std::span<u32> shown);

} // namespace vd
