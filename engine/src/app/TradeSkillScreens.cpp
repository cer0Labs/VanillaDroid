#include "app/TradeSkillScreens.h"

#include "app/ChatLinkInsert.h"
#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ChatLinks.h"
#include "game/GameData.h"
#include "game/SocialText.h"
#include "game/SpellText.h"
#include "game/Travel.h"
#include "net/UpdateFields.gen.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;

constexpr u32 kHeaderColor = ui::rgba(255, 209, 0); // TradeSkillTypeColor["header"]
constexpr u32 kEffectCreateItem = 24;               // SPELL_EFFECT_CREATE_ITEM
constexpr std::string_view kRed = "|cffff2020";     // RED_FONT_COLOR_CODE
constexpr std::string_view kClose = "|r";           // FONT_COLOR_CODE_CLOSE
constexpr u32 kMaxCount = 100;                      // TradeSkillFrameIncrement_OnClick's limit

const ui::WidgetId kCountField = ui::widgetId("tradeskill.count");

// BuildColoredListString: "Anvil, |cffff2020Blacksmith Hammer|r" (what is missing in red), appended to `out`.
void appendListItem(std::string& out, std::string_view name, bool met) {
    if (!out.empty()) {
        out.append(", ");
    }
    if (!met) {
        out.append(kRed);
    }
    out.append(name);
    if (!met) {
        out.append(kClose);
    }
}

std::string_view copyInto(std::span<char> out, std::string_view s) {
    const usize n = std::min(s.size(), out.size() - 1);
    std::memcpy(out.data(), s.data(), n);
    out[n] = '\0';
    return {out.data(), n};
}

} // namespace

void TradeSkillScreens::init(const game::GameData* data, const game::AbilityData* abilities,
                             UiResources* resources) {
    m_data = data;
    m_abilities = abilities;
    m_resources = resources;
    const char* trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY");
    m_trace = trace && trace[0] == '1';
    m_rows.reserve(300);
    m_craftRows.reserve(300);
    m_craftTexts.reserve(300);
    m_reagents.reserve(8);
    m_choices.reserve(32);
    m_choiceValues.reserve(32);
    m_description.reserve(512);
}

void TradeSkillScreens::trace(const char* format, ...) const {
    if (!m_trace) {
        return;
    }
    char text[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    VD_LOG_INFO("Professions: %s", text);
}

bool TradeSkillScreens::tradeSkillOpen(const GameSession& session) {
    return session.play().tradeSkills().tradeSkillOpen();
}

bool TradeSkillScreens::craftOpen(const GameSession& session) {
    return session.play().tradeSkills().craftOpen();
}

void TradeSkillScreens::closeTradeSkill(GameSession& session) {
    session.play().tradeSkills().closeTradeSkill();
    m_menu = Menu::None;
}

void TradeSkillScreens::closeCraft(GameSession& session) {
    session.play().tradeSkills().closeCraft();
}

ItemView TradeSkillScreens::item(GameSession& session, u32 entry) {
    ItemView view;
    view.entry = entry;
    if (const net::ItemQueryInfo* info = session.item(entry)) {
        view.name = info->name;
        view.quality = info->quality;
        view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
    } else {
        view.name = "...";
    }
    return view;
}

void TradeSkillScreens::buildDetail(GameSession& session, u32 spellId, bool craft, u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::TradeSkills& skills = play.tradeSkills();
    const game::TradeSkillData* data = skills.data();
    m_detail = RecipeDetailView{};
    m_reagents.clear();
    game::TradeSpellInfo spell;
    if (!data || !data->spell(spellId, spell)) {
        return;
    }
    m_detail.name = spell.name;
    const bool makesItem = spell.createdItem != 0 && (!craft || spell.effects[0] == kEffectCreateItem);
    if (makesItem) {
        // GetTradeSkillIcon: the product's icon; its count (GetTradeSkillNumMade) on it.
        const ItemView product = item(session, spell.createdItem);
        m_detail.iconHash = product.iconHash;
        m_detail.itemEntry = spell.createdItem;
    }
    if (m_detail.iconHash == 0 && m_data) {
        const std::optional<game::SpellInfo> info = m_data->spell(spellId); // GetCraftIcon: the spell's
        m_detail.iconHash = info ? info->iconHash : 0;
    }
    if (!craft && spell.madeMax() > 1) {
        const u32 low = spell.madeMin(), high = spell.madeMax();
        if (low == high) {
            std::snprintf(m_made.data(), m_made.size(), "%u", low);
        } else {
            std::snprintf(m_made.data(), m_made.size(), "%u-%u", low, high);
            if (std::strlen(m_made.data()) > 4) { // wider than the icon: "~average"
                std::snprintf(m_made.data(), m_made.size(), "~%u", (low + high) / 2);
            }
        }
        m_detail.made = m_made.data();
    }

    // GetTradeSkillTools / GetCraftSpellFocus: the focus (never red: the client lists it as met) and the
    // tools carried.
    std::string list;
    if (spell.spellFocus != 0) {
        appendListItem(list, data->focusName(spell.spellFocus), true);
    }
    for (const u32 tool : spell.tools) {
        if (tool == 0) {
            continue;
        }
        const net::ItemQueryInfo* info = session.item(tool);
        appendListItem(list, info ? std::string_view(info->name) : std::string_view("..."),
                       play.inventory().count(tool) > 0);
    }
    if (craft && list.empty()) {
        // Beast Training: the pet level the ability needs (TRAINER_PET_LEVEL, red when the pet is lower).
        u32 points = 0, level = 0;
        if (skills.craftPetCost(spellId, points, level) && level > 0) {
            const game::Entity* pet = session.entities().find(play.abilities().pet().guid);
            const u32 petLevel = pet ? pet->value(uf::UNIT_FIELD_LEVEL) : 0;
            text::format(m_requirements, petLevel >= level ? "TRAINER_PET_LEVEL" : "TRAINER_PET_LEVEL_RED",
                         level);
            m_detail.requirements = m_requirements.data();
        }
    } else if (!list.empty()) {
        m_detail.requirements = copyInto(m_requirements, list);
    }

    if (!craft) {
        // GetTradeSkillCooldown: COOLDOWN_REMAINING and SecondsToTime.
        const u32 remaining = play.spells().remaining(spellId, nowMs);
        if (remaining > 0 && remaining != 0xFFFFFFFFu) {
            char time[64];
            const std::string_view left = game::secondsToTime(time, (remaining + 999) / 1000);
            std::snprintf(m_cooldown.data(), m_cooldown.size(), "%.*s %.*s",
                          static_cast<int>(text::get("COOLDOWN_REMAINING").size()),
                          text::get("COOLDOWN_REMAINING").data(), static_cast<int>(left.size()), left.data());
            m_detail.cooldown = m_cooldown.data();
        }
    } else {
        // GetCraftInfo's training point cost (COSTS_LABEL ... TRAINING_POINTS_LABEL, the number red when
        // short) and GetCraftDescription.
        u32 points = 0, level = 0;
        if (skills.craftPetCost(spellId, points, level) && points > 0) {
            const bool enough = skills.petTrainingPoints().available() >= static_cast<i32>(points);
            const std::string_view costLabel = text::get("COSTS_LABEL");
            const std::string_view pointsLabel = text::get("TRAINING_POINTS_LABEL");
            std::snprintf(m_cost.data(), m_cost.size(), "%.*s %s%u%s %.*s",
                          static_cast<int>(costLabel.size()), costLabel.data(), enough ? "" : "|cffff2020",
                          points, enough ? "" : "|r", static_cast<int>(pointsLabel.size()),
                          pointsLabel.data());
            m_detail.cost = m_cost.data();
        }
        if (m_abilities) {
            const game::Entity* me = session.entities().find(session.playerGuid());
            game::SpellTextContext context;
            context.casterLevel = me ? me->value(uf::UNIT_FIELD_LEVEL) : 0;
            context.gender = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) : 0;
            if (game::spellDescription(m_abilities->spellText(), spellId, m_description, context)) {
                m_detail.description = m_description;
            }
        }
    }

    // GetTradeSkillReagentInfo: name, icon, needed and carried.
    for (u32 i = 0; i < spell.reagents.size(); ++i) {
        if (spell.reagents[i] == 0 || spell.reagentCounts[i] == 0) {
            continue;
        }
        ReagentView r;
        r.item = item(session, spell.reagents[i]);
        r.need = spell.reagentCounts[i];
        r.have = play.inventory().count(spell.reagents[i]);
        m_reagents.push_back(r);
    }
    m_detail.reagents = m_reagents;
}

void TradeSkillScreens::castCraft(GameSession& session, u32 spellId) {
    // DoCraft: Spell_C_CastSpell(spell). A craft that needs an item (Enchanting's "Enchant Bracer - ...",
    // TARGET_FLAG_ITEM) starts the item targeting there (game::SpellTargeting: the bags light what it takes).
    trace("craft %u", spellId);
    session.play().castSpell(spellId, 0);
}

void TradeSkillScreens::syncCount(ui::TextInput& input) {
    if (!m_editing) {
        return;
    }
    if (!input.active() || input.field() != kCountField) {
        m_editing = false; // closed (Back) or another field took the keyboard
        return;
    }
    u32 value = 0;
    bool digits = false;
    for (const char ch : input.text()) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + static_cast<u32>(ch - '0');
            digits = true;
        }
    }
    if (digits) {
        m_count = std::max(1u, value); // OnTextChanged: "0" becomes "1"
    }
    if (input.submitted()) {
        input.end(); // OnEnterPressed: ClearFocus
        m_editing = false;
    }
}

void TradeSkillScreens::drawTradeSkill(GameSession& session, DialogContext& c, ui::TextInput& input,
                                       u32 nowMs) {
    VD_PROFILE_ZONE("TradeSkillScreens::tradeSkill");
    game::Gameplay& play = session.play();
    game::TradeSkills& skills = play.tradeSkills();
    if (!skills.tradeSkillOpen()) {
        return;
    }
    // TradeSkillInputBox: the repeat count of the selection (TradeSkillFrame_SetSelection,
    // UPDATE_TRADESKILL_RECAST).
    if (skills.selected() != m_countSelected) {
        m_countSelected = skills.selected();
        m_count = skills.repeatCount();
    }
    if (skills.repeatCount() != m_countRepeat) {
        m_countRepeat = skills.repeatCount();
        m_count = m_countRepeat;
    }
    syncCount(input);

    const u32 cooldownSecond =
        skills.selected() != 0 ? play.spells().remaining(skills.selected(), nowMs) / 1000 : 0;
    if (skills.version() != m_tradeVersion || play.queries().version() != m_tradeItems ||
        play.inventory().version() != m_tradeBags || cooldownSecond != m_tradeCooldownSecond) {
        m_tradeVersion = skills.version();
        m_tradeItems = play.queries().version();
        m_tradeBags = play.inventory().version();
        m_tradeCooldownSecond = cooldownSecond;
        m_rows.clear();
        for (const game::TradeSkillRow& row : skills.rows()) {
            RecipeRowView v;
            v.header = row.header;
            if (row.header) {
                const game::TradeHeader& h = skills.headers()[row.index];
                v.name = h.name;
                v.color = kHeaderColor;
                v.expanded = !h.collapsed;
            } else {
                const game::TradeRecipe& r = skills.recipes()[row.index];
                v.name = r.name;
                v.color = recipeRowColor(r.color);
                v.available = r.available;
            }
            m_rows.push_back(v);
        }
        TradeSkillView& v = m_tradeView;
        v = TradeSkillView{};
        v.title = text::format(m_title, "TRADE_SKILL_TITLE", skills.skillLineName());
        v.rankName = skills.skillLineName();
        v.rows = m_rows;
        v.hasRows = !m_rows.empty();
        v.allCollapsed = skills.allCollapsed();
        // The drop-downs' texts: "All" when every choice is on and there are several, else the one chosen.
        const std::span<const game::TradeHeader> headers = skills.headers();
        if (skills.subclassFilter() >= 0) {
            v.subclassText = headers[static_cast<u32>(skills.subclassFilter())].name;
        } else {
            v.subclassText = headers.size() == 1 ? headers[0].name : text::get("ALL_SUBCLASSES");
        }
        const u32 slots = skills.slotsPresent();
        u32 slotCount = 0, onlySlot = 0;
        for (u32 i = 0; i < game::kTradeSlots; ++i) {
            if (slots & (1u << i)) {
                ++slotCount;
                onlySlot = i;
            }
        }
        if (skills.slotFilter() >= 0) {
            v.slotText = text::get(game::kTradeSlotKeys[static_cast<u32>(skills.slotFilter())]);
        } else {
            v.slotText =
                slotCount == 1 ? text::get(game::kTradeSlotKeys[onlySlot]) : text::get("ALL_INVENTORY_SLOTS");
        }
        const game::TradeRecipe* selected = skills.selectedRecipe();
        v.selected = skills.selectedRow();
        v.hasDetail = selected != nullptr;
        if (selected) {
            buildDetail(session, selected->spellId, false, nowMs);
            v.detail = m_detail;
            v.canCreate = std::all_of(m_reagents.begin(), m_reagents.end(),
                                      [](const ReagentView& r) { return r.have >= r.need; });
        }
    }
    // The skill rank moves while the window is open (SKILL_LINES_CHANGED).
    m_tradeView.rank = skills.skillRank();
    m_tradeView.maxRank = skills.skillMax();
    m_tradeView.countEditing = m_editing;
    std::snprintf(m_countText.data(), m_countText.size(), "%u", m_count);
    m_tradeView.countText = m_countText.data();

    const TradeSkillResult r = tradeSkillWindow(c, m_tradeView);
    const bool linking = c.chatLink && c.chatLink->active();
    if (r.select >= 0) {
        skills.selectRow(static_cast<u32>(r.select));
    }
    if (r.toggleAll) {
        skills.setAllCollapsed(!skills.allCollapsed()); // TradeSkillCollapseAllButton_OnClick
    }
    if (r.subclassMenu) {
        m_menu = Menu::Subclass;
    }
    if (r.slotMenu) {
        m_menu = Menu::Slot;
    }
    if (r.decrement && m_count > 1) {
        --m_count;
    }
    if (r.increment && m_count < kMaxCount) {
        ++m_count;
    }
    if ((r.decrement || r.increment || r.create || r.createAll) && m_editing) {
        input.end(); // the buttons ClearFocus the box
        m_editing = false;
    }
    if (r.countTapped) {
        if (m_editing) {
            input.end();
            m_editing = false;
        } else {
            std::snprintf(m_countText.data(), m_countText.size(), "%u", m_count);
            input.begin(kCountField, m_countText.data(), 3); // letters="3"
            m_editing = true;
        }
    }
    const game::TradeRecipe* selected = skills.selectedRecipe();
    if (r.createAll && selected) {
        m_count = std::max(1u, selected->available); // TradeSkillInputBox:SetNumber(numAvailable)
        trace("create all %u x %u", selected->spellId, selected->available);
        skills.create(selected->available);
    } else if (r.create && selected) {
        trace("create %u x %u", selected->spellId, m_count);
        skills.create(m_count);
    }
    if (linking && (r.productTapped || r.reagentTapped >= 0) && selected) {
        // Shift + click: GetTradeSkillItemLink / GetTradeSkillReagentItemLink into the chat.
        game::TradeSpellInfo spell;
        const game::TradeSkillData* data = skills.data();
        u32 entry = 0;
        if (data && data->spell(selected->spellId, spell)) {
            if (r.productTapped) {
                entry = spell.createdItem;
            } else if (static_cast<usize>(r.reagentTapped) < m_reagents.size()) {
                entry = m_reagents[static_cast<usize>(r.reagentTapped)].item.entry;
            }
        }
        if (const net::ItemQueryInfo* info = entry != 0 ? session.item(entry) : nullptr) {
            char link[256];
            c.chatLink->insert(game::formatItemLink(
                link, entry, game::itemQualityColor(static_cast<u8>(info->quality)), info->name));
        }
    }
    if (r.closed) {
        if (m_editing) {
            input.end();
            m_editing = false;
        }
        closeTradeSkill(session);
        return;
    }
    if (m_menu != Menu::None) {
        menu(session, c);
    }
}

void TradeSkillScreens::menu(GameSession& session, DialogContext& c) {
    game::TradeSkills& skills = session.play().tradeSkills();
    m_choices.clear();
    m_choiceValues.clear();
    std::string_view title;
    if (m_menu == Menu::Subclass) {
        // TradeSkillFilterFrame_LoadSubClasses: "All Subclasses" first when there are several.
        const std::span<const game::TradeHeader> headers = skills.headers();
        const bool all = skills.subclassFilter() < 0;
        if (headers.size() > 1) {
            m_choices.push_back({text::get("ALL_SUBCLASSES"), ui::kWhite, all, true});
            m_choiceValues.push_back(-1);
        }
        for (u32 i = 0; i < headers.size(); ++i) {
            m_choices.push_back({headers[i].name, ui::kWhite,
                                 skills.subclassFilter() == static_cast<i32>(i) || headers.size() == 1,
                                 true});
            m_choiceValues.push_back(static_cast<i32>(i));
        }
        title = m_tradeView.subclassText;
    } else {
        const u32 slots = skills.slotsPresent();
        u32 count = 0;
        for (u32 i = 0; i < game::kTradeSlots; ++i) {
            count += (slots >> i) & 1u;
        }
        if (count > 1) {
            m_choices.push_back(
                {text::get("ALL_INVENTORY_SLOTS"), ui::kWhite, skills.slotFilter() < 0, true});
            m_choiceValues.push_back(-1);
        }
        for (u32 i = 0; i < game::kTradeSlots; ++i) {
            if (slots & (1u << i)) {
                // FINGER0SLOT / TRINKET0SLOT name the ring and trinket choices; the four bag bits are one.
                if (i >= 20 && i <= 22) {
                    continue;
                }
                m_choices.push_back({text::get(game::kTradeSlotKeys[i]), ui::kWhite,
                                     skills.slotFilter() == static_cast<i32>(i) || count == 1, true});
                m_choiceValues.push_back(static_cast<i32>(i));
            }
        }
        title = m_tradeView.slotText;
    }
    DialogContext modal = c;
    modal.panel = {}; // the list: a small modal panel over the window (UIDropDownMenu's list)
    const i32 picked = choiceList(modal, "tradeskill.menu", title, m_choices);
    if (picked >= 0 && static_cast<usize>(picked) < m_choiceValues.size()) {
        const i32 value = m_choiceValues[static_cast<usize>(picked)];
        if (m_menu == Menu::Subclass) {
            trace("subclass filter %d", value);
            skills.setSubclassFilter(value); // SetTradeSkillSubClassFilter
        } else {
            trace("slot filter %d", value);
            skills.setSlotFilter(value); // SetTradeSkillInvSlotFilter
        }
    }
    if (picked != -1) {
        m_menu = Menu::None;
    }
}

void TradeSkillScreens::drawCraft(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("TradeSkillScreens::craft");
    game::Gameplay& play = session.play();
    game::TradeSkills& skills = play.tradeSkills();
    if (!skills.craftOpen()) {
        return;
    }
    if (skills.version() != m_craftVersion || play.queries().version() != m_craftItems ||
        play.inventory().version() != m_craftBags) {
        m_craftVersion = skills.version();
        m_craftItems = play.queries().version();
        m_craftBags = play.inventory().version();
        const std::span<const game::CraftEntry> crafts = skills.crafts();
        m_craftRows.clear();
        m_craftTexts.resize(crafts.size());
        for (u32 i = 0; i < crafts.size(); ++i) {
            const game::CraftEntry& e = crafts[i];
            RecipeRowView v;
            v.header = e.spellId == 0;
            v.name = e.name;
            if (v.header) {
                v.color = kHeaderColor;
                v.expanded = !e.collapsed;
            } else {
                v.color = recipeRowColor(e.color);
                v.available = e.available;
                if (!e.rank.empty()) {
                    v.sub = text::format(m_craftTexts[i].sub, "PARENS_TEMPLATE", e.rank); // CraftNSubText
                }
                u32 points = 0, level = 0;
                if (skills.craftPetCost(e.spellId, points, level) && points > 0) {
                    v.cost = text::format(m_craftTexts[i].cost, "TRAINER_LIST_TP", points); // CraftNCost
                }
            }
            m_craftRows.push_back(v);
        }
        CraftView& v = m_craftView;
        v = CraftView{};
        const std::string_view opener =
            m_abilities ? m_abilities->spellText().name(skills.craftOpener()) : std::string_view{};
        v.title = copyInto(m_title, opener); // GetCraftName
        if (skills.craftSkillLine() != 0 && skills.data()) {
            v.rankName = skills.data()->skillLineName(skills.craftSkillLine());
            v.rank = skills.craftSkillRank();
            v.maxRank = skills.craftSkillMax();
        }
        v.rows = m_craftRows;
        v.headers = skills.craftHeaders();
        v.allCollapsed = skills.craftsAllCollapsed();
        switch (skills.craftList()) { // GetCraftButtonToken
        case game::kCraftBeastTraining: v.button = text::get("TRAIN"); break;
        case game::kCraftDisguise: v.button = text::get("DISGUISE"); break;
        case game::kCraftEnchanting: v.button = text::get("ENSCRIBE"); break;
        default: v.button = text::get("USE"); break;
        }
        const game::PetTrainingPoints points = skills.petTrainingPoints();
        v.trainingPoints = points.total > 0 ? points.available() : -1; // Craft_UpdateTrainingPoints
        v.selected = skills.selectedCraftRow();
        v.hasDetail = v.selected >= 0;
        if (v.hasDetail) {
            const game::CraftEntry& e = crafts[static_cast<u32>(v.selected)];
            buildDetail(session, e.spellId, true, nowMs);
            v.detail = m_detail;
            v.canCraft = e.color != game::RecipeColor::Used &&
                         std::all_of(m_reagents.begin(), m_reagents.end(),
                                     [](const ReagentView& r) { return r.have >= r.need; });
        }
    }
    const CraftResult r = craftWindow(c, m_craftView);
    if (r.select >= 0) {
        skills.selectCraftRow(static_cast<u32>(r.select));
    }
    if (r.toggleAll) {
        skills.setCraftsAllCollapsed(!skills.craftsAllCollapsed());
    }
    if (r.craft && skills.selectedCraft() != 0) {
        castCraft(session, skills.selectedCraft());
    }
    if (c.chatLink && c.chatLink->active() && (r.productTapped || r.reagentTapped >= 0) &&
        skills.selectedCraft()) {
        // Shift + click: GetCraftItemLink (an Enchanting craft links as "enchant:<spell>", one that makes an
        // item as the item) / GetCraftReagentItemLink.
        char link[256];
        game::TradeSpellInfo spell;
        const game::TradeSkillData* data = skills.data();
        if (data && data->spell(skills.selectedCraft(), spell)) {
            u32 entry = 0;
            if (r.productTapped && spell.castUi == game::kCraftEnchanting) {
                std::snprintf(link, sizeof(link), "|cffffffff|Henchant:%u|h[%.*s]|h|r", spell.id,
                              static_cast<int>(spell.name.size()), spell.name.data());
                c.chatLink->insert(link);
            } else if (r.productTapped && spell.effects[0] == kEffectCreateItem) {
                entry = spell.createdItem;
            } else if (r.reagentTapped >= 0 && static_cast<usize>(r.reagentTapped) < m_reagents.size()) {
                entry = m_reagents[static_cast<usize>(r.reagentTapped)].item.entry;
            }
            if (const net::ItemQueryInfo* info = entry != 0 ? session.item(entry) : nullptr) {
                c.chatLink->insert(game::formatItemLink(
                    link, entry, game::itemQualityColor(static_cast<u8>(info->quality)), info->name));
            }
        }
    }
    if (r.closed) {
        closeCraft(session);
    }
}

} // namespace vd
