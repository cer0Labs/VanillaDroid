#include "app/PopupGallery.h"

#include "core/Log.h"
#include "core/Text.h"

namespace vd {
namespace {

std::string_view unit(u32 n, bool minutes) {
    return text::get(minutes ? (n == 1 ? "MINUTES" : "MINUTES_P1") : (n == 1 ? "SECONDS" : "SECONDS_P1"));
}

// StaticPopup_OnUpdate's countdown text of `key` (with nothing, or `name`, before the time).
std::string_view countdownText(std::span<char> out, text::Key key, i32 ms, std::string_view name = {}) {
    const u32 seconds = ms <= 0 ? 0u : static_cast<u32>((ms + 999) / 1000);
    const bool minutes = seconds >= 60;
    const u32 n = minutes ? (seconds + 59) / 60 : seconds;
    return name.empty() ? text::format(out, key, n, unit(n, minutes))
                        : text::format(out, key, name, n, unit(n, minutes));
}

} // namespace

void PopupGallery::fill(u32 set) {
    m_stack.clear();
    PopupOutcome discarded;
    while (m_stack.takeOutcome(discarded)) {
    }
    if (set == 0) {
        if (Popup* p = m_stack.show(PopupKind::Death, true)) {
            p->data2 = 1;
            p->timeLeftMs = 6 * 60 * 1000 - 1000;
        }
        if (Popup* p = m_stack.show(PopupKind::PartyInvite, true)) {
            p->setName("Thrall");
        }
        m_stack.show(PopupKind::DuelRequested, false, 1);
        if (Popup* p = m_stack.show(PopupKind::ConfirmTalentWipe, false)) {
            p->money = 10000; // one gold, the first reset
        }
    } else if (set == 1) {
        // CAMP first: it closes what logging out may close (its rule), the rest stays.
        m_stack.show(PopupKind::Camp, false);
        if (Popup* p = m_stack.show(PopupKind::DeleteGoodItem, true)) {
            p->setName("Thunderfury, Blessed Blade of the Windseeker");
            p->button1Enabled = false;
        }
        if (Popup* p = m_stack.show(PopupKind::ResurrectNoSickness, true)) {
            p->setName("Aeryn");
            p->startDelayMs = 25 * 1000; // the corpse delay: Accept waits
            p->timeLeftMs = p->startDelayMs + 60 * 1000;
        }
        if (Popup* p = m_stack.show(PopupKind::ConfirmSummon, false)) {
            p->setName("Galen");
            p->setName2("Elwynn Forest");
            p->timeLeftMs = 2 * 60 * 1000;
        }
    } else if (set == 3) { // phase 12: the instances' popups
        m_stack.show(PopupKind::ConfirmResetInstances, false);
        m_stack.show(PopupKind::RecoverCorpseInstance, true);
        if (Popup* p = m_stack.show(PopupKind::InstanceBoot, false)) {
            p->setName("Goldshire"); // GetBindLocation
            p->timeLeftMs = 60 * 1000;
        }
        if (Popup* p = m_stack.show(PopupKind::ConfirmPetUnlearn, false)) { // phase 15 (pets)
            p->money = 1000; // a pet's first untraining: 10 silver
        }
    } else {
        if (Popup* p = m_stack.show(PopupKind::XpLoss, true)) {
            char duration[32];
            p->setName(text::format(duration, "GENERIC_MIN_P1", 10)); // level 20 and above
        }
        if (Popup* p = m_stack.show(PopupKind::GuildInvite, false)) {
            p->setName("Galen");
            p->setName2("Knights of Goldshire");
        }
        m_stack.show(PopupKind::Trade, false);
    }
    m_filled = true;
}

void PopupGallery::draw(DialogContext& c, u32 set, u64 nowMs) {
    if (!m_filled || set != m_set) {
        m_set = set;
        fill(set);
        m_lastMs = nowMs;
    }
    m_stack.update(static_cast<u32>(nowMs - m_lastMs));
    m_lastMs = nowMs;
    if (!m_stack.any()) { // everything answered: the set again a little later
        m_emptySinceMs = m_emptySinceMs == 0 ? nowMs : m_emptySinceMs;
        if (nowMs - m_emptySinceMs > 4000) {
            m_emptySinceMs = 0;
            fill(set);
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
        std::span<char> out = m_texts[i];
        switch (p.kind) {
        case PopupKind::Death: v.text = countdownText(out, info.text, p.timeLeftMs); break;
        case PopupKind::Camp: v.text = countdownText(out, info.text, p.timeLeftMs); break;
        case PopupKind::PartyInvite: v.text = text::format(out, info.text, p.nameView()); break;
        case PopupKind::DuelRequested: v.text = text::format(out, info.text, "Arthas"); break;
        case PopupKind::XpLoss:
            v.text = text::format(out, p.again ? info.againText : info.text, p.nameView());
            break;
        case PopupKind::DeleteGoodItem: v.text = text::format(out, info.text, p.nameView()); break;
        case PopupKind::ResurrectNoSickness:
            v.text = p.startDelayMs > 0 ? countdownText(out, info.delayText, p.startDelayMs, p.nameView())
                                        : text::format(out, info.text, p.nameView());
            break;
        case PopupKind::ConfirmSummon: {
            const u32 seconds = static_cast<u32>((p.timeLeftMs + 999) / 1000);
            const bool minutes = seconds >= 60;
            const u32 n = minutes ? (seconds + 59) / 60 : seconds;
            v.text = text::format(out, info.text, p.nameView(), p.name2View(), n, unit(n, minutes));
            break;
        }
        case PopupKind::GuildInvite:
            v.text = text::format(out, info.text, p.nameView(), p.name2View());
            break;
        case PopupKind::Trade: v.text = text::format(out, info.text, "Galen"); break;
        case PopupKind::InstanceBoot: v.text = countdownText(out, info.text, p.timeLeftMs, p.nameView()); break;
        default: v.text = text::format(out, info.text); break;
        }
        v.button1 = info.button1.name.empty() ? std::string_view{} : text::get(info.button1);
        v.button2 = info.button2.name.empty()    ? std::string_view{}
                    : p.kind == PopupKind::Death ? std::string_view("Reincarnation") // the spell's own name
                                                 : text::get(info.button2);
        v.button1Enabled = PopupStack::button1Ready(p);
        v.alert = info.has(PopupInfo::kShowAlert);
        v.money = info.has(PopupInfo::kMoneyFrame);
        v.copper = p.money;
        v.editBox = info.has(PopupInfo::kEditBox);
        v.editText = "DEL";
    }
    const PopupTaps taps = popupStack(c, m_views, m_heights);
    if (taps.slot >= 0) {
        m_stack.click(static_cast<u32>(taps.slot), taps.button);
    }
    PopupOutcome o;
    while (m_stack.takeOutcome(o)) {
        VD_LOG_INFO("Popup gallery: kind %u ended %u", static_cast<u32>(o.kind), static_cast<u32>(o.end));
    }
}

} // namespace vd
