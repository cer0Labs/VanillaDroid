#include "app/TargetingGallery.h"

#include "app/TargetingWindows.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"

namespace vd {
namespace {

constexpr u64 iconHash(const char* name) {
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

constexpr u32 kRangeBlinkMs = 2500; // the place page shows "out of range" every other period

// A rogue's backpack while Instant Poison waits for a weapon: what the spell suits is lit, the rest dimmed.
struct Sample {
    const char* icon;
    u32 count;
    u32 quality;
    bool lit;
};
constexpr Sample kBackpack[16] = {
    {"INV_Misc_Rune_01", 1, 1, false},
    {"INV_Potion_19", 5, 1, false}, // the poison itself: dimmed
    {"INV_Weapon_ShortBlade_05", 1, 2, true},
    {"INV_Sword_04", 1, 1, true},
    {"INV_Chest_Cloth_05", 1, 1, false},
    {"INV_Misc_Food_15", 5, 1, false},
    {"INV_Mace_01", 1, 2, true},
    {nullptr, 0, 0, false},
    {"INV_Weapon_Bow_02", 1, 1, false},
    {"INV_Fabric_Linen_01", 7, 1, false},
    {nullptr, 0, 0, false},
    {"INV_Box_01", 1, 1, false},
    {nullptr, 0, 0, false},
    {nullptr, 0, 0, false},
    {"INV_Misc_Bone_06", 2, 0, false},
    {nullptr, 0, 0, false},
};

} // namespace

void TargetingGallery::popups(DialogContext& c, u64 nowMs) {
    if (!m_filled) {
        m_stack.clear();
        PopupOutcome discarded;
        while (m_stack.takeOutcome(discarded)) {
        }
        // Exclusive: the second replaces the first in the real stack; here one after the other.
        m_stack.show(PopupKind::ReplaceEnchant, false);
        m_filled = true;
        m_lastMs = nowMs;
    }
    m_stack.update(static_cast<u32>(nowMs - m_lastMs));
    m_lastMs = nowMs;
    if (!m_stack.any()) { // answered: BIND_ENCHANT next, then the replacement again
        m_emptySinceMs = m_emptySinceMs == 0 ? nowMs : m_emptySinceMs;
        if (nowMs - m_emptySinceMs > 1500) {
            m_emptySinceMs = 0;
            m_stack.show((nowMs / 1000) % 2 ? PopupKind::BindEnchant : PopupKind::ReplaceEnchant, false);
        }
    }
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        const Popup& p = m_stack.slots()[i];
        PopupView& v = m_views[i];
        v = PopupView{};
        if (!p.shown()) {
            continue;
        }
        const PopupInfo& info = popupInfo(p.kind);
        v.text = p.kind == PopupKind::BindEnchant
                     ? text::format(m_texts[i], info.text)
                     : text::format(m_texts[i], info.text, std::string_view("Deadly Poison"),
                                    std::string_view("Instant Poison"));
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
        v.alert = info.has(PopupInfo::kShowAlert);
    }
    const PopupTaps taps = popupStack(c, m_views, m_heights);
    if (taps.slot >= 0) {
        m_stack.click(static_cast<u32>(taps.slot), taps.button);
    }
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        VD_LOG_INFO("Targeting gallery: popup %u ended %u", static_cast<u32>(o.kind),
                    static_cast<u32>(o.end));
    }
}

void TargetingGallery::draw(DialogContext& c, u32 page, u64 nowMs) {
    TargetingBarView bar;
    if (page == 0) {
        // Blizzard waiting for its place.
        bar.iconHash = iconHash("Spell_Frost_IceStorm");
        bar.title = "Blizzard";
        bar.showCast = true;
        bar.inRange = (nowMs / kRangeBlinkMs) % 2 == 0;
        bar.hint = text::get(bar.inRange ? "VD_TARGETING_PLACE" : "SPELL_FAILED_OUT_OF_RANGE");
        popups(c, nowMs);
    } else {
        // Instant Poison waiting for a weapon: the bags, its marks.
        bar.iconHash = iconHash("INV_Potion_19");
        bar.title = "Instant Poison";
        bar.hint = text::get("VD_TARGETING_ITEM");
        m_drag.track(c.ui.touches());
        m_cells.clear();
        for (u32 i = 0; i < 16; ++i) {
            const Sample& s = kBackpack[i];
            ItemSlotView v;
            v.ref = {ItemRef::Kind::Slot, 255, static_cast<u8>(23 + i), 0};
            if (s.icon) {
                v.empty = false;
                v.iconHash = iconHash(s.icon);
                v.count = s.count;
                v.quality = s.quality;
                v.lit = s.lit;
                v.dimmed = !s.lit;
            }
            m_cells.push_back(v);
        }
        const BagSectionView section{text::get("BACKPACK_TOOLTIP"), 0, 16, false};
        BagsView view;
        view.sections = {&section, 1};
        view.cells = m_cells;
        view.money = 12345;
        view.freeSlots = 5;
        view.totalSlots = 16;
        const ItemWindowResult r = bagsWindow(c, view, m_drag);
        m_drag.finish();
        if (r.tapped.valid()) {
            VD_LOG_INFO("Targeting gallery: item %u/%u", r.tapped.bag, r.tapped.slot);
        }
    }
    const TargetingBarResult r = targetingBar(c, bar);
    if (r.cast || r.cancel) {
        VD_LOG_INFO("Targeting gallery: %s", r.cast ? "cast" : "cancel");
    }
}

} // namespace vd
