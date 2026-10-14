// Phase 15 (items): InventoryScreens' ContainerFrames (the same class as InventoryScreens.cpp): which bags
// are open (app/ContainerLayout's ContainerStack, driven by the bag bar, OpenAllBags, BAG_OPEN and
// BAG_CLOSED), their views from game::Inventory, where they stack in the bags' place and what their buttons
// do.

#include "app/InventoryScreens.h"

#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/ItemActions.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

namespace uf = net::uf;
using Zone = game::ItemPlace::Kind;
constexpr Zone kContainerZones[] = {Zone::Bags}; // a drop on a frame, off its buttons
constexpr u64 kKeyringIcon = assetPathHash("Interface\\ContainerFrame\\KeyRing-Bag-Icon.blp");
// SMSG_OPEN_CONTAINER comes as the server moves the bag, before its update fields put it in the slot: it
// waits this long for it.
constexpr u32 kOpenWaitMs = 3000;

// The bit of a container in the open-frames mask (the backpack, the bags, the keyring).
u32 containerBit(i32 id) {
    return id == kKeyringContainer ? 1u << 5 : id >= 0 && id < 5 ? 1u << static_cast<u32>(id) : 0u;
}

} // namespace

ContainerSizes InventoryScreens::containerSizes(GameSession& session) const {
    const game::Inventory& inv = session.play().inventory();
    ContainerSizes sizes;
    for (i32 c = game::kContainerBackpack; c <= game::kContainerLastBag; ++c) {
        sizes.bags[static_cast<u32>(c)] = inv.containerSize(c);
    }
    const game::Entity* me = session.entities().find(session.playerGuid());
    const u32 level = me ? me->value(uf::UNIT_FIELD_LEVEL) : 1;
    sizes.keyring = std::min(keyringSize(level), inv.containerSize(game::kContainerKeyring));
    return sizes;
}

bool InventoryScreens::containerShown(i32 id) const {
    return m_containers.isOpen(id) && (m_containersShown & containerBit(id)) != 0;
}

void InventoryScreens::bagButton(GameSession& session, i32 button, bool shift) {
    const ContainerSizes sizes = containerSizes(session);
    const i32 id = button == 5 ? kKeyringContainer : button;
    if (!shift && m_containers.isOpen(id) && !containerShown(id)) {
        m_containers.raise(id); // open but out of sight (no room): it comes back as the newest
        m_containersShown |= containerBit(id);
        return;
    }
    if (shift && button >= 0 && button <= 4) {
        // MainMenuBarBackpackButton's Shift + click and BagSlotButton_OnShiftClick (when the slot holds a
        // bag).
        if (button == 0 || sizes.bags[static_cast<u32>(button)] > 0) {
            m_containers.openAllBags(sizes);
        }
    } else if (button == 0) {
        m_containers.toggleBackpack(sizes); // BackpackButton_OnClick
    } else if (button >= 1 && button <= 4) {
        m_containers.toggleBag(button, sizes); // BagSlotButton_OnClick
        m_uiSound(UiSound::BagMenuButton);
    } else if (button == 5) {
        m_containers.toggleKeyring(sizes); // KeyRingButton
    }
    if (m_trace) {
        VD_LOG_INFO("Bags: button %d%s -> %u frames open", button, shift ? " (all)" : "",
                    static_cast<u32>(m_containers.order().size()));
    }
}

void InventoryScreens::openAllBags(GameSession& session, bool force) {
    m_containers.openAllBags(containerSizes(session), force);
    const game::Inventory& inv = session.play().inventory();
    if (inv.banker() != 0) { // OpenAllBags with the BankFrame shown: the bank bags too (ToggleBag(5..10))
        u32 all = 0;
        for (i32 c = game::kContainerFirstBankBag; c <= game::kContainerLastBankBag; ++c) {
            all |= inv.containerSize(c) > 0 ? 1u << static_cast<u32>(c - game::kContainerFirstBankBag) : 0u;
        }
        m_bankFrames = m_bankFrames == all && !force ? 0u : all;
    }
}

void InventoryScreens::dropOnBagBar(GameSession& session, const DialogContext& c,
                                    const std::array<ui::Rect, 6>& rects) {
    m_drag.track(c.ui.touches());
    if (!m_drag.active || !m_drag.released || m_drag.taken) {
        return;
    }
    const game::Inventory& inv = session.play().inventory();
    for (u32 i = 0; i < rects.size(); ++i) {
        if (rects[i].w <= 0.0f || !m_drag.droppedIn(rects[i])) {
            continue;
        }
        game::ItemPlace to;
        if (i == 0) {
            to = game::ItemPlace::of(game::ItemPlace::Kind::Backpack); // PutItemInBackpack
        } else if (i <= 4) {
            to = game::ItemPlace::slot({255, static_cast<u8>(18 + i)}); // PutItemInBag: a bag swaps in
        } else { // PutKeyInKeyRing: the first empty slot of the keyring
            const ContainerSizes sizes = containerSizes(session);
            for (u32 k = 0; k < sizes.keyring && to.kind == game::ItemPlace::Kind::None; ++k) {
                if (!inv.containerItem(game::kContainerKeyring, k)) {
                    to = game::ItemPlace::slot(inv.containerPos(game::kContainerKeyring, k));
                }
            }
        }
        m_drag.taken = true;
        if (to.kind != game::ItemPlace::Kind::None &&
            !(m_popups && m_popups->askBindForDrop(session, place(m_drag.from), to))) {
            game::dropItem(session.play(), place(m_drag.from), to);
        }
        return;
    }
}

bool InventoryScreens::releasedItemAt(const DialogContext& c, f32& x, f32& y) {
    m_drag.track(c.ui.touches());
    if (!m_drag.active || !m_drag.released || m_drag.taken) {
        return false;
    }
    x = m_drag.x;
    y = m_drag.y;
    return true;
}

std::string_view InventoryScreens::dropOnUnit(GameSession& session, u64 unit) {
    // Phase 16 (pullouts): a player of the player's side, not itself (the raid's members are); the pet's food is
    // another path of the same C function that no frame here offers.
    if (!m_drag.active || !m_drag.released || m_drag.taken || unit == 0 || unit == session.playerGuid()) {
        return {};
    }
    m_drag.taken = true;
    game::Gameplay& play = session.play();
    game::Trade& trade = play.exchange().trade();
    const game::ItemPlace from = place(m_drag.from);
    if (trade.partner() == unit && trade.open()) {
        const i32 free = trade.freeSlot();
        if (free >= 0 && from.kind == game::ItemPlace::Kind::Slot) {
            game::dropItem(play, from, game::ItemPlace::of(game::ItemPlace::Kind::Trade, static_cast<u32>(free)));
        }
        return {};
    }
    if (trade.phase() != game::TradePhase::None) {
        return {}; // trading with somebody else: nothing happens
    }
    const std::string_view problem = trade.initiateProblem(unit);
    if (problem.empty()) {
        trade.initiate(unit);
    }
    return problem;
}

void InventoryScreens::containerOpened(u64 guid, u32 nowMs) {
    m_openGuid = guid;
    m_openMs = nowMs;
}

bool InventoryScreens::updateContainers(GameSession& session, u32 nowMs) {
    const game::Inventory& inv = session.play().inventory();
    // BAG_CLOSED: a bag slot whose bag went away or changed closes that bag's frame.
    for (i32 id = 1; id <= game::kContainerLastBag; ++id) {
        const game::ItemInstance* bag = inv.bagOf(id);
        const u64 guid = bag ? bag->guid : 0;
        u64& shown = m_frameBags[static_cast<u32>(id - 1)];
        if (guid != shown) {
            if (shown != 0) {
                m_containers.closeBag(id);
            }
            shown = guid;
        }
    }
    // BAG_OPEN: the bag just equipped (SMSG_OPEN_CONTAINER) once it is in its slot; the player's guid: the
    // backpack.
    bool opened = false;
    if (m_openGuid != 0) {
        const ContainerSizes sizes = containerSizes(session);
        i32 found = m_openGuid == session.playerGuid() ? 0 : -1;
        for (i32 id = 1; id <= game::kContainerLastBag && found < 0; ++id) {
            const game::ItemInstance* bag = inv.bagOf(id);
            found = bag && bag->guid == m_openGuid ? id : found;
        }
        if (found >= 0) {
            opened = !m_containers.isOpen(found) && sizes.size(found) > 0;
            m_containers.openBag(found, sizes);
            m_openGuid = 0;
        } else if (nowMs - m_openMs > kOpenWaitMs) {
            m_openGuid = 0; // a bank bag, or a bag that never showed up
        }
    }
    // A frame whose bag has no slots any more closes too (the keyring stays).
    const ContainerSizes sizes = containerSizes(session);
    for (i32 id = 1; id <= game::kContainerLastBag; ++id) {
        if (m_containers.isOpen(id) && sizes.size(id) == 0) {
            m_containers.closeBag(id);
        }
    }
    // ContainerFrame_OnShow / OnHide: igBackPackOpen / igBackPackClose, KeyRingOpen / KeyRingClose for the
    // keyring.
    u32 mask = 0;
    for (const i32 id : m_containers.order()) {
        mask |= containerBit(id);
    }
    const u32 shownNow = mask & ~m_containerMask, hiddenNow = m_containerMask & ~mask;
    if (hiddenNow & ~containerBit(kKeyringContainer)) {
        m_uiSound(UiSound::BagClose);
    }
    if (hiddenNow & containerBit(kKeyringContainer)) {
        m_uiSound(UiSound::KeyRingClose);
    }
    if (shownNow & ~containerBit(kKeyringContainer)) {
        m_uiSound(UiSound::BagOpen);
    }
    if (shownNow & containerBit(kKeyringContainer)) {
        m_uiSound(UiSound::KeyRingOpen);
    }
    m_containerMask = mask;
    return opened;
}

void InventoryScreens::drawContainers(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("InventoryScreens::containers");
    const game::Inventory& inv = session.play().inventory();
    const ContainerSizes sizes = containerSizes(session);
    // The views, in the order the frames opened (ContainerFrame1.bags).
    m_containerViews.clear();
    m_containerCells.clear();
    std::array<u32, kContainerFrames> first{};
    u32 count = 0;
    for (const i32 id : m_containers.order()) {
        if (count == kContainerFrames) {
            break;
        }
        const u32 size = sizes.size(id);
        first[count] = static_cast<u32>(m_containerCells.size());
        for (u32 i = 0; i < size && m_containerCells.size() < m_containerCells.capacity(); ++i) {
            m_containerCells.push_back(
                slotView(session, inv.containerItem(id, i), inv.containerPos(id, i), nowMs));
        }
        ContainerFrameView view;
        view.id = id;
        if (id == kKeyringContainer) {
            view.name = text::get("KEYRING");
            view.portrait = kKeyringIcon; // SetPortraitToTexture(KeyRing-Bag-Icon)
        } else if (id == game::kContainerBackpack) {
            view.name = text::get("BACKPACK_TOOLTIP"); // GetBagName(0)
            view.money = inv.money();
        } else if (const game::ItemInstance* bag = inv.bagOf(id)) {
            const net::ItemQueryInfo* info = session.item(bag->entry);
            view.name = info ? std::string_view(info->name) : std::string_view("...");
            view.portrait = info && m_data ? m_data->itemIconHash(info->displayId) : 0;
        }
        m_containerViews.push_back(view);
        m_containerHeights[count] = containerGeometry(id, size).height;
        ++count;
    }
    for (u32 i = 0; i < count; ++i) { // the cells' spans once they no longer move
        const u32 end = i + 1 < count ? first[i + 1] : static_cast<u32>(m_containerCells.size());
        m_containerViews[i].cells =
            std::span<const ItemSlotView>(m_containerCells).subspan(first[i], end - first[i]);
    }
    if (count == 0) {
        return;
    }
    // Where they go: from the bottom right of the bags' place, as large as the other panels' item buttons,
    // shrunk (CONTAINER_SCALE) to fit down to the HUD's own unit, the original's size. No room even then
    // (they never cover the stick or the action buttons): the oldest stay open out of sight until there is
    // room again.
    const kit::Metrics m = kit::metrics(c);
    const ui::Rect area = c.panel.w > 0.0f ? c.panel : c.safeArea;
    const f32 largest = m.item / 37.0f;
    const f32 smallest = std::min(largest, c.ui.px(ui::kUnitDp));
    std::array<u32, kContainerFrames> shown{};
    const u32 visible = shownContainers({m_containerHeights.data(), count}, area, smallest, shown);
    m_containersShown = 0;
    for (u32 i = 0; i < visible; ++i) { // the ones shown, in order (shown[i] >= i: compacted in place)
        m_containerViews[i] = m_containerViews[shown[i]];
        m_containerHeights[i] = m_containerHeights[shown[i]];
        m_containersShown |= containerBit(m_containerViews[i].id);
    }
    const std::span<const f32> heights{m_containerHeights.data(), visible};
    const f32 scale = containerScale(heights, area, largest, smallest);
    layoutContainers(heights, area, scale, m_containerRects);
    ItemWindowResult items;
    ContainerFramesResult r;
    containerFrames(c, std::span<const ContainerFrameView>(m_containerViews).first(visible),
                    {m_containerRects.data(), visible}, scale, m_drag, items, r);
    apply(session, items, kContainerZones, items.tappedRect);
    if (r.closed != ContainerFramesResult::kNone) {
        m_containers.closeBag(r.closed); // ToggleBag(this:GetParent():GetID())
    }
    if (r.portrait >= 1 && r.portrait <= game::kContainerLastBag) { // the bag's tooltip (SetInventoryItem)
        openTooltip({ItemRef::Kind::Slot, 255, static_cast<u8>(18 + r.portrait), 0}, r.portraitRect);
    }
}

} // namespace vd
