#pragma once

#include "core/Text.h"
#include "core/Types.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace vd::game {

class Inventory;
class ItemData;
class QueryCache;
class SpellBook;
struct ItemInstance;

// Tooltip colors (RGBA8, red in the low byte: the ui's rgba layout), as the client's GameTooltip uses them.
constexpr u32 tooltipColor(u8 r, u8 g, u8 b) {
    return u32{r} | (u32{g} << 8) | (u32{b} << 16) | 0xFF000000u;
}
constexpr u32 kTooltipWhite = tooltipColor(255, 255, 255);  // HIGHLIGHT_FONT_COLOR
constexpr u32 kTooltipGold = tooltipColor(255, 209, 0);     // NORMAL_FONT_COLOR: flavour text, set names
constexpr u32 kTooltipGreen = tooltipColor(26, 255, 26);    // GREEN_FONT_COLOR: Use / Equip / enchantments
constexpr u32 kTooltipRed = tooltipColor(255, 26, 26);      // RED_FONT_COLOR: unmet requirements
constexpr u32 kTooltipGray = tooltipColor(128, 128, 128);   // set bonuses not active, set pieces not worn
constexpr u32 kTooltipSetPiece = tooltipColor(255, 255, 153); // set pieces worn
// ITEM_QUALITY_COLORS (0 poor .. 6 artifact).
u32 itemQualityColor(u32 quality);

// One line of an item tooltip: text on the left, optionally on the right (slot / type, speed).
struct TooltipLine {
    std::string_view left;
    std::string_view right;
    u32 leftColor = kTooltipWhite;
    u32 rightColor = kTooltipWhite;
    bool wrap = false; // a long text that wraps to the tooltip's width (spell texts, flavour text)
    u32 money = 0;     // > 0: coins drawn after the text (the sell price)
    bool gapBefore = false; // a little space above (the set section)
};

// An item's tooltip as lines of text, built into fixed storage (rebuild it when the item or the query cache
// changes; nothing allocates once the reusable string inside has grown).
class ItemTooltip {
public:
    static constexpr u32 kMaxLines = 48;

    void clear();
    std::span<const TooltipLine> lines() const { return {m_lines.data(), m_count}; }
    u32 quality() const { return m_quality; }
    bool empty() const { return m_count == 0; }

    // Building (copies the text in). Lines beyond kMaxLines or text beyond the storage are dropped.
    TooltipLine& add(std::string_view text, u32 color = kTooltipWhite);
    // A text of the table (core/Text: ITEM_*...) formatted with `args`.
    template <typename... Args> TooltipLine& addText(u32 color, text::Key key, const Args&... args) {
        char buffer[1024];
        return add(text::format(buffer, key, args...), color);
    }
    void right(TooltipLine& line, std::string_view text, u32 color = kTooltipWhite);
    void setQuality(u32 quality) { m_quality = quality; }
    std::string& scratch() { return m_scratch; } // spell texts are expanded here first

private:
    std::string_view store(std::string_view text);

    std::array<TooltipLine, kMaxLines> m_lines{};
    u32 m_count = 0;
    std::array<char, 8192> m_text{};
    u32 m_used = 0;
    u32 m_quality = 1;
    TooltipLine m_overflow; // where lines past kMaxLines go
    std::string m_scratch;
};

// What the tooltip knows about its reader. Every pointer may be null (fewer lines then: no spell texts without
// `data`, no set pieces worn without `inventory`...).
struct TooltipContext {
    u32 level = 0; // the player's level, class and race (requirements turn red when unmet)
    u32 cls = 0;
    u32 race = 0;
    const Inventory* inventory = nullptr; // proficiencies and the set pieces worn
    QueryCache* queries = nullptr;        // creators' names and set pieces' names (asked for when missing)
    const ItemData* data = nullptr;       // spell texts, sets, enchantments, skill and faction names
    const SpellBook* spells = nullptr;    // required spells known
    bool showSellPrice = true;
};

// The client's GameTooltip for an item (SetBagItem / SetInventoryItem / SetHyperlink), with the client's texts
// (core/Text: ITEM_*, INVTYPE_*, DAMAGE_TEMPLATE...) and names from its tables (ItemData): name in its quality color
// (with its random suffix), binding, unique, slot and type, damage / speed / dps, armor, block, stats, resistances,
// enchantments, durability, classes and races, requirements (red when unmet), "Use:" / "Equip:" / "Chance on hit:"
// lines with the spell's text, charges, the set, flavour text in quotes, the creator and the sell price. `instance`:
// the item as owned (null for a template only: loot, vendor, quest rewards).
void buildItemTooltip(const net::ItemQueryInfo& item, const ItemInstance* instance, const TooltipContext& context,
                      ItemTooltip& out);

// Names the windows share, from the texts table: an inventory type (INVTYPE_*: "One-Hand", "Head"...; empty for
// none), a reputation rank (0 hated .. 7 exalted: FACTION_STANDING_LABEL*). Item subclass names are
// ItemData::subclassName's.
std::string_view inventoryTypeName(u32 inventoryType);
std::string_view reputationRankName(u32 rank);

} // namespace vd::game
