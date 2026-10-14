#include "app/OptionsGallery.h"

#include "core/Log.h"

#include <iterator>

namespace vd {

void OptionsGallery::init(const game::GameData* data, UiResources* resources) {
    m_macros.init(data, resources);
}

void OptionsGallery::draw(DialogContext& c, ui::TextInput& text, Page page) {
    if (!m_started || page != m_page) {
        m_started = true;
        m_page = page;
        // Sample macros: a few of each tab, as a warrior would keep them.
        game::MacroStore& store = m_macros.store();
        store.clear();
        store.create(game::MacroTab::Account, "Charge", "Ability_Warrior_Charge", "/cast Charge\n/say Charge!");
        store.create(game::MacroTab::Account, "Heal me", "Spell_Holy_Heal", "/cast Lesser Heal(Rank 2)\n/p Healing!");
        store.create(game::MacroTab::Account, "Dance", "Spell_Shadow_Charm", "/dance\n/say Party time");
        store.create(game::MacroTab::Account, "Mount", "Ability_Mount_RidingHorse", "/cast Summon Charger");
        store.create(game::MacroTab::Account, "Bandage", "INV_Misc_Bandage_15", "/script UseContainerItem(0, 1)");
        store.create(game::MacroTab::Character, "Rend", "Ability_Gouge", "/cast Rend(Rank 3)");
        m_macros.setCharacterName("Arthel");
        m_macros.open();
        if (page == Page::MacroPopup) {
            m_macros.showPopup(false);
        }
        m_settings = SettingsView{};
        m_options.open(page == Page::SoundOptions ? OptionsScreens::Window::Sound : OptionsScreens::Window::Interface,
                       nullptr, m_settings);
        m_options.setTab(page == Page::UiOptionsAdvanced ? 1u : 0u);
    }
    switch (page) {
    case Page::Macro:
    case Page::MacroPopup:
        if (!m_macros.draw(nullptr, c, text)) {
            m_macros.open(); // the gallery's page stays
        }
        break;
    case Page::UiOptions:
    case Page::UiOptionsAdvanced:
    case Page::SoundOptions: {
        const OptionsScreens::Window window =
            page == Page::SoundOptions ? OptionsScreens::Window::Sound : OptionsScreens::Window::Interface;
        if (!m_options.draw(window, nullptr, c, m_settings)) {
            VD_LOG_INFO("Gallery: options closed");
            m_options.open(window, nullptr, m_settings);
        }
        break;
    }
    case Page::CombatText: drawCombatText(c); break;
    }
}

void OptionsGallery::drawCombatText(DialogContext& c) {
    using Kind = CombatTextScreens::Kind;
    SettingsView s;
    s.showCombatText = true;
    s.combatTextAuraFade = s.combatTextDodgeParryMiss = s.combatTextResistances = s.combatTextReputation = true;
    s.combatTextReactives = s.combatTextFriendlyNames = s.combatTextComboPoints = s.combatTextMana = true;
    const u32 now = static_cast<u32>(c.ui.nowMs());
    s.combatTextFloatMode = 1 + (now / 8000u) % 3u;
    struct Sample {
        Kind kind;
        const char* text;
        i32 amount, partial;
    };
    static constexpr Sample kSamples[] = {
        {Kind::Damage, "", 37, 0},        {Kind::Heal, "Aeryn", 120, 0},     {Kind::Dodge, "", 0, 0},
        {Kind::DamageCrit, "", 84, 0},    {Kind::AuraStart, "Power Word: Fortitude", 0, 0},
        {Kind::Rage, "", 12, 0},          {Kind::SpellDamage, "", 56, 0},    {Kind::Resist, "", 41, 14},
        {Kind::Miss, "", 0, 0},           {Kind::HealCrit, "", 260, 0},      {Kind::Faction, "Stormwind", 25, 0},
        {Kind::Block, "", 0, 0},          {Kind::EnteringCombat, "", 0, 0},  {Kind::ComboPoints, "", 3, 0},
        {Kind::SpellActive, "Overpower", 0, 0}, {Kind::AuraEndHarmful, "Rend", 0, 0}, {Kind::HealthLow, "", 0, 0},
        {Kind::Mana, "", 250, 0},         {Kind::Parry, "", 0, 0},           {Kind::LeavingCombat, "", 0, 0},
    };
    if (now >= m_nextTextMs) {
        m_nextTextMs = now + 300;
        const Sample& sample = kSamples[m_textIndex++ % std::size(kSamples)];
        m_combatText.add(sample.kind, s, sample.text, sample.amount, sample.partial, now);
    }
    m_combatText.draw(c, s, now);
}

} // namespace vd
