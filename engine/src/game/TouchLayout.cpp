#include "game/TouchLayout.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace vd::game {
namespace {

constexpr std::string_view kMagic = "VDTOUCH 3";   // places in hundredths of the screen's height
constexpr std::string_view kMagicV2 = "VDTOUCH 2"; // the same, a Target button 6 across by default (older layouts)
constexpr std::string_view kMagicV1 = "VDTOUCH 1"; // places in millimetres (the oldest layouts)
constexpr f32 kTargetSizeV2 = 6.0f;

constexpr std::string_view kPlaceNames[kTouchPlaces] = {
    "main",   "a",     "b",        "c",        "d",      "e",      "f",         "g",      "h",   "c1", "c2",
    "c3",     "jump",  "mount",    "reactive", "interact", "target", "templates", "drawer", "pet", "classbar"};
constexpr std::string_view kContextNames[kTouchContexts] = {"base",   "battle",  "defensive", "berserker",
                                                            "stealth", "cat",     "prowl",     "bear",
                                                            "moonkin", "travel",  "shadowform", "melee"};
constexpr std::string_view kKindNames[] = {"empty", "spell", "item", "macro", "group", "pet", "consumable", "special"};
constexpr std::string_view kConsumableNames[] = {"health", "mana", "rage", "energy", "bandage", "food", "drink"};
constexpr std::string_view kSpecialNames[] = {"attack", "interact", "mount", "pet", "forms", "drawer"};
constexpr std::string_view kProfileNames[kTouchProfiles] = {"square", "wide"};
static_assert(std::size(kConsumableNames) == static_cast<usize>(TouchConsumable::Count));
static_assert(std::size(kSpecialNames) == static_cast<usize>(TouchSpecial::Count));

template <usize N>
bool findName(const std::string_view (&names)[N], std::string_view name, u32& out) {
    for (u32 i = 0; i < N; ++i) {
        if (names[i] == name) {
            out = i;
            return true;
        }
    }
    return false;
}

// The next space-separated word of `line` (a quoted one keeps its spaces), advancing it.
std::string_view word(std::string_view& line) {
    usize i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    line.remove_prefix(i);
    if (line.empty()) {
        return {};
    }
    if (line.front() == '"') {
        const usize end = line.find('"', 1);
        const std::string_view w = line.substr(1, end == std::string_view::npos ? line.size() - 1 : end - 1);
        line.remove_prefix(end == std::string_view::npos ? line.size() : end + 1);
        return w;
    }
    usize end = 0;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
        ++end;
    }
    const std::string_view w = line.substr(0, end);
    line.remove_prefix(end);
    return w;
}

bool number(std::string_view text, u32& out) {
    return !text.empty() && std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{};
}

bool number(std::string_view text, f32& out) {
    // std::from_chars for floats is missing from VS 2019's library in some modes: strtof on a bounded copy.
    char buffer[32];
    if (text.empty() || text.size() >= sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    char* end = nullptr;
    out = std::strtof(buffer, &end);
    return end == buffer + text.size();
}

void appendf(std::string& out, const char* format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) {
        out.append(line, static_cast<usize>(std::min(n, static_cast<int>(sizeof(line)) - 1)));
    }
}

void formatAction(std::string& out, const TouchAction& a) {
    if (a.hidden()) {
        out += " hidden";
        if (a.manual()) {
            out += " manual";
        }
        return;
    }
    out += ' ';
    out += kKindNames[static_cast<u32>(a.kind)];
    switch (a.kind) {
    case TouchActionKind::Consumable:
        out += ' ';
        out += a.id < std::size(kConsumableNames) ? kConsumableNames[a.id] : std::string_view("health");
        break;
    case TouchActionKind::Special:
        out += ' ';
        out += a.id < std::size(kSpecialNames) ? kSpecialNames[a.id] : std::string_view("attack");
        break;
    default: appendf(out, " %u", a.id); break;
    }
    if (a.manual()) {
        out += " manual";
    }
    if ((a.flags & kTouchFixedRank) != 0) {
        out += " rank";
    }
}

// "<kind> <id> [manual] [rank]" or "hidden": `kind` its first word, `line` the rest.
bool parseAction(std::string_view kind, std::string_view line, TouchAction& out) {
    out = {};
    if (kind == "hidden") {
        out.flags = kTouchHidden;
        for (std::string_view flag = word(line); !flag.empty(); flag = word(line)) {
            if (flag == "manual") {
                out.flags |= kTouchManual;
            }
        }
        return true;
    }
    u32 k = 0;
    if (!findName(kKindNames, kind, k) || k == 0) {
        return false;
    }
    out.kind = static_cast<TouchActionKind>(k);
    const std::string_view id = word(line);
    bool ok = false;
    if (out.kind == TouchActionKind::Consumable) {
        ok = findName(kConsumableNames, id, out.id);
    } else if (out.kind == TouchActionKind::Special) {
        ok = findName(kSpecialNames, id, out.id);
    } else {
        ok = number(id, out.id);
    }
    if (!ok) {
        return false;
    }
    for (std::string_view flag = word(line); !flag.empty(); flag = word(line)) {
        if (flag == "manual") {
            out.flags |= kTouchManual;
        } else if (flag == "rank") {
            out.flags |= kTouchFixedRank;
        }
    }
    return true;
}

// Phase 20 (pets, §6.8): "demon <spell>" (the warlock's last demon) and, per hunter pet family,
// "petfood <family> accepted <item>... refused <item>...".
void formatPets(std::string& out, const TouchLayout& layout) {
    if (layout.lastDemon != 0) {
        appendf(out, "demon %u\n", layout.lastDemon);
    }
    for (const TouchPetFood::Family& f : layout.petFood.families) {
        if (f.family == 0) {
            continue;
        }
        appendf(out, "petfood %u accepted", f.family);
        for (const u32 entry : f.accepted) {
            if (entry != 0) {
                appendf(out, " %u", entry);
            }
        }
        out += " refused";
        for (const u32 entry : f.refused) {
            if (entry != 0) {
                appendf(out, " %u", entry);
            }
        }
        out += '\n';
    }
}

void parsePets(std::string_view key, std::string_view line, TouchLayout& out) {
    if (key == "demon") {
        number(word(line), out.lastDemon);
        return;
    }
    u32 family = 0;
    if (!number(word(line), family) || family == 0) {
        return;
    }
    TouchPetFood::Family* f = nullptr;
    for (TouchPetFood::Family& slot : out.petFood.families) {
        if (slot.family == 0 || slot.family == family) {
            f = &slot;
            break;
        }
    }
    if (!f) {
        return;
    }
    *f = {};
    f->family = family;
    bool refused = false;
    u32 accepted = 0, refusedCount = 0;
    for (std::string_view w = word(line); !w.empty(); w = word(line)) {
        u32 entry = 0;
        if (w == "accepted" || w == "refused") {
            refused = w == "refused";
        } else if (number(w, entry) && entry != 0) {
            if (refused && refusedCount < f->refused.size()) {
                f->refused[refusedCount++] = entry;
            } else if (!refused && accepted < f->accepted.size()) {
                f->accepted[accepted++] = entry;
            }
        }
    }
}

} // namespace

std::string_view TouchTemplate::nameView() const {
    return {name.data(), ::strnlen(name.data(), name.size())};
}

void TouchTemplate::setName(std::string_view text) {
    name = {};
    const usize n = std::min(text.size(), name.size() - 1);
    for (usize i = 0; i < n; ++i) {
        // The file quotes the name: a quote in it becomes an apostrophe, a line break a space.
        const char c = text[i];
        name[i] = c == '"' ? '\'' : (c == '\n' || c == '\r') ? ' ' : c;
    }
}

TouchGeometry defaultTouchGeometry(TouchProfile profile) {
    // A reference picture of a mobile game (a whole 16:9 screen, docs/MOBILE_CONTROLS.md §12) measured in hundredths of
    // the screen's height: Main 12.4 across, 24 from the right edge and 23 over the bottom; the ring 9.5, the column
    // and the consumables 8; the whole cluster about 53 high and 65 wide. Both profiles keep these proportions; only
    // the class bar differs (a row on square screens, one button with its fan on wide ones, where a row would reach the
    // minimap).
    TouchGeometry g;
    auto set = [&g](TouchPlace place, f32 x, f32 y, f32 size) { g.places[touchIndex(place)] = {x, y, size}; };
    g.mainRight = 24.0f;
    g.mainBottom = 23.0f;
    set(TouchPlace::Main, 0.0f, 0.0f, 12.4f);
    set(TouchPlace::RingA, 0.4f, 20.6f, 9.5f);
    set(TouchPlace::RingB, 14.7f, 20.6f, 9.5f);
    set(TouchPlace::RingC, -12.4f, 15.4f, 9.5f);
    set(TouchPlace::RingD, -20.3f, 0.6f, 9.5f);
    set(TouchPlace::RingE, -20.0f, -13.8f, 9.5f);
    set(TouchPlace::ColumnF, -32.0f, 11.0f, 8.3f);
    set(TouchPlace::ColumnG, -33.3f, -2.4f, 8.3f);
    set(TouchPlace::ColumnH, -33.3f, -13.8f, 8.3f);
    set(TouchPlace::Consumable1, -17.8f, 32.4f, 8.0f);
    set(TouchPlace::Consumable2, -6.5f, 32.4f, 8.0f);
    set(TouchPlace::Consumable3, 4.6f, 32.4f, 8.0f);
    set(TouchPlace::Jump, -3.3f, -13.8f, 7.0f);     // under Main, a little to the left (the reference's)
    set(TouchPlace::Mount, 14.8f, 4.3f, 7.0f);      // right of Main (the reference's horse)
    set(TouchPlace::Reactive, 8.0f, 11.0f, 8.0f);   // over Main, between it and the ring's A and B
    set(TouchPlace::Interact, -11.0f, -8.0f, 7.0f); // down-left of Main
    set(TouchPlace::Target, -25.0f, 24.0f, 7.0f);   // as Jump
    set(TouchPlace::Templates, 14.8f, 32.4f, 6.0f); // the reference's gear, right of the consumables
    set(TouchPlace::Drawer, -29.0f, 32.4f, 6.0f);
    set(TouchPlace::Pet, -36.0f, 24.0f, 7.0f);
    if (profile == TouchProfile::Square) {
        set(TouchPlace::ClassBar, 4.6f, 41.0f, 6.0f); // a row from here to the left, over the consumables
    } else {
        set(TouchPlace::ClassBar, -38.0f, 32.4f, 6.0f); // one button: its fan
    }
    return g;
}

TouchTemplate makeTouchTemplate(std::string_view name) {
    TouchTemplate t;
    t.setName(name);
    for (u32 p = 0; p < kTouchProfiles; ++p) {
        t.geometry[p] = defaultTouchGeometry(static_cast<TouchProfile>(p));
    }
    return t;
}

TouchAction resolveTouchAction(const TouchTemplate& t, TouchContext context, u32 slot) {
    if (slot >= kTouchSlots) {
        return {};
    }
    const u32 c = static_cast<u32>(context);
    if (c != 0 && c < kTouchContexts && t.layerEnabled(context)) {
        const TouchAction& own = t.layers[c][slot];
        if (own.hidden()) {
            return {};
        }
        if (!own.empty()) {
            return own;
        }
    }
    const TouchAction& base = t.layers[0][slot];
    return base.hidden() ? TouchAction{} : base;
}

std::string formatTouchLayout(const TouchLayout& layout) {
    std::string out;
    out.reserve(4096);
    out += kMagic;
    out += '\n';
    appendf(out, "active %u\n", layout.active);
    if (!layout.mount.empty()) {
        out += "mount";
        formatAction(out, layout.mount);
        out += '\n';
    }
    formatPets(out, layout); // phase 20 (pets)
    for (u32 i = 0; i < std::min<u32>(layout.count, kMaxTouchTemplates); ++i) {
        const TouchTemplate& t = layout.templates[i];
        out += "template \"";
        out += t.nameView();
        out += "\"\n";
        appendf(out, "  role %u\n  site %u\n  disabled %u\n", t.role, t.site, t.disabledLayers);
        if (t.healerFrames) { // phase 20 (assists)
            out += "  healerframes 1\n";
        }
        if (t.classBarGroups[0] != 0) {
            out += "  classgroups";
            for (const u8 g : t.classBarGroups) {
                appendf(out, " %u", g);
            }
            out += '\n';
        }
        for (u32 p = 0; p < kTouchProfiles; ++p) {
            const TouchGeometry& g = t.geometry[p];
            appendf(out, "  geometry %.*s %.3f %.2f %.2f\n", static_cast<int>(kProfileNames[p].size()),
                    kProfileNames[p].data(), g.scale, g.mainRight, g.mainBottom);
            for (u32 e = 0; e < kTouchPlaces; ++e) {
                const TouchPlacement& at = g.places[e];
                appendf(out, "    %.*s %.2f %.2f %.2f\n", static_cast<int>(kPlaceNames[e].size()), kPlaceNames[e].data(),
                        at.x, at.y, at.size);
            }
        }
        for (u32 c = 0; c < kTouchContexts; ++c) {
            bool any = false;
            for (const TouchAction& a : t.layers[c]) {
                any = any || !a.empty() || a.hidden();
            }
            if (!any) {
                continue;
            }
            out += "  layer ";
            out += kContextNames[c];
            out += '\n';
            for (u32 s = 0; s < kTouchSlots; ++s) {
                const TouchAction& a = t.layers[c][s];
                if (a.empty() && !a.hidden()) {
                    continue;
                }
                out += "    ";
                out += kPlaceNames[s];
                formatAction(out, a);
                out += '\n';
            }
        }
        for (u32 gi = 0; gi < kTouchGroups; ++gi) {
            const TouchGroup& g = t.groups[gi];
            if (g.empty() && g.autoKind == 0) {
                continue;
            }
            appendf(out, "  group %u auto %u current %u%s\n", gi, g.autoKind, g.current,
                    (g.flags & kTouchManual) != 0 ? " manual" : "");
            for (u32 a = 0; a < std::min<u32>(g.count, TouchGroup::kMaxActions); ++a) {
                out += "   ";
                formatAction(out, g.actions[a]);
                out += '\n';
            }
        }
        out += "end\n";
    }
    return out;
}

bool parseTouchLayout(std::string_view text, TouchLayout& out) {
    out = {};
    const bool millimetres = text.substr(0, kMagicV1.size()) == kMagicV1;
    const bool version2 = text.substr(0, kMagicV2.size()) == kMagicV2;
    if (!millimetres && !version2 && text.substr(0, kMagic.size()) != kMagic) {
        return false;
    }
    u32 active = 0;
    TouchTemplate* t = nullptr;
    i32 profile = -1;     // the geometry being read
    i32 layer = -1;       // the layer being read
    TouchGroup* group = nullptr;
    usize pos = 0;
    while (pos < text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view key = word(line);
        if (key.empty() || key == "VDTOUCH") {
            continue;
        }
        if (key == "active") {
            number(word(line), active);
        } else if (key == "mount" && !t) {
            const std::string_view kind = word(line);
            TouchAction a;
            if (parseAction(kind, line, a) && (a.kind == TouchActionKind::Item || a.kind == TouchActionKind::Spell)) {
                out.mount = a;
            }
        } else if ((key == "demon" || key == "petfood") && !t) { // phase 20 (pets)
            parsePets(key, line, out);
        } else if (key == "template") {
            if (out.count >= kMaxTouchTemplates) {
                t = nullptr;
                continue;
            }
            t = &out.templates[out.count++];
            *t = makeTouchTemplate(word(line));
            profile = layer = -1;
            group = nullptr;
        } else if (!t) {
            continue;
        } else if (key == "end") {
            t = nullptr;
        } else if (key == "role" || key == "site" || key == "disabled") {
            u32 v = 0;
            number(word(line), v);
            if (key == "role") {
                t->role = static_cast<u8>(v);
            } else if (key == "site") {
                t->site = static_cast<u8>(std::min<u32>(v, static_cast<u32>(TouchSite::Count) - 1));
            } else {
                t->disabledLayers = static_cast<u16>(v);
            }
        } else if (key == "healerframes") { // phase 20 (assists)
            u32 v = 0;
            number(word(line), v);
            t->healerFrames = v != 0;
        } else if (key == "classgroups") {
            for (u8& g : t->classBarGroups) {
                u32 v = 0;
                number(word(line), v);
                g = static_cast<u8>(v <= kTouchGroups ? v : 0);
            }
        } else if (key == "geometry") {
            u32 p = 0;
            profile = findName(kProfileNames, word(line), p) ? static_cast<i32>(p) : -1;
            layer = -1;
            group = nullptr;
            if (profile >= 0) {
                TouchGeometry& g = t->geometry[static_cast<u32>(profile)];
                f32 scale = 1.0f, right = g.mainRight, bottom = g.mainBottom;
                number(word(line), scale);
                number(word(line), right);
                number(word(line), bottom);
                g.scale = std::clamp(scale, 0.5f, 1.6f);
                g.mainRight = right;
                g.mainBottom = bottom;
            }
        } else if (key == "layer") {
            u32 c = 0;
            layer = findName(kContextNames, word(line), c) ? static_cast<i32>(c) : -1;
            profile = -1;
            group = nullptr;
        } else if (key == "group") {
            u32 index = 0;
            group = number(word(line), index) && index < kTouchGroups ? &t->groups[index] : nullptr;
            profile = layer = -1;
            if (group) {
                *group = {};
                for (std::string_view w = word(line); !w.empty(); w = word(line)) {
                    u32 v = 0;
                    if (w == "auto" && number(word(line), v)) {
                        group->autoKind = static_cast<u8>(v);
                    } else if (w == "current" && number(word(line), v)) {
                        group->current = static_cast<u8>(v);
                    } else if (w == "manual") {
                        group->flags |= kTouchManual;
                    }
                }
            }
        } else if (group) {
            // A group's action: the line is "<kind> <id> ..." (key is the kind).
            TouchAction a;
            if (group->count < TouchGroup::kMaxActions && parseAction(key, line, a) && !a.hidden() &&
                a.kind != TouchActionKind::Group) {
                group->actions[group->count++] = a;
            }
        } else if (profile >= 0) {
            TouchPlace place;
            TouchPlacement at;
            if (touchPlaceByName(key, place) && number(word(line), at.x) && number(word(line), at.y) &&
                number(word(line), at.size)) {
                t->geometry[static_cast<u32>(profile)].places[touchIndex(place)] = at;
            }
        } else if (layer >= 0) {
            TouchPlace place;
            TouchAction a;
            const std::string_view kind = word(line); // before `line` is passed on (the arguments' order is open)
            if (touchPlaceByName(key, place) && isTouchSlot(place) && parseAction(kind, line, a)) {
                t->layers[static_cast<u32>(layer)][touchIndex(place)] = a;
            }
        }
    }
    for (u32 i = 0; i < out.count; ++i) {
        for (TouchGroup& g : out.templates[i].groups) {
            g.current = g.count > 0 ? static_cast<u8>(std::min<u32>(g.current, g.count - 1u)) : 0;
        }
    }
    out.active = out.count > 0 ? std::min(active, out.count - 1) : 0;
    if (version2) {
        // A file of the second version: a Target button of the old default size grows to the new one (as Jump's).
        for (u32 i = 0; i < out.count; ++i) {
            for (u32 p = 0; p < kTouchProfiles; ++p) {
                TouchPlacement& target = out.templates[i].geometry[p].places[touchIndex(TouchPlace::Target)];
                if (std::abs(target.size - kTargetSizeV2) < 0.01f) {
                    target.size = defaultTouchGeometry(static_cast<TouchProfile>(p))
                                      .places[touchIndex(TouchPlace::Target)]
                                      .size;
                }
            }
        }
    }
    if (millimetres) {
        // A file of the first version: its places were millimetres; the proportions of the screen now (the defaults).
        for (u32 i = 0; i < out.count; ++i) {
            for (u32 p = 0; p < kTouchProfiles; ++p) {
                out.templates[i].geometry[p] = defaultTouchGeometry(static_cast<TouchProfile>(p));
            }
        }
    }
    return true;
}

std::string_view touchPlaceName(TouchPlace place) {
    const u32 i = touchIndex(place);
    return i < kTouchPlaces ? kPlaceNames[i] : std::string_view{};
}

std::string_view touchContextName(TouchContext context) {
    const u32 i = static_cast<u32>(context);
    return i < kTouchContexts ? kContextNames[i] : std::string_view{};
}

bool touchPlaceByName(std::string_view name, TouchPlace& out) {
    u32 i = 0;
    if (!findName(kPlaceNames, name, i)) {
        return false;
    }
    out = static_cast<TouchPlace>(i);
    return true;
}

bool touchContextByName(std::string_view name, TouchContext& out) {
    u32 i = 0;
    if (!findName(kContextNames, name, i)) {
        return false;
    }
    out = static_cast<TouchContext>(i);
    return true;
}

} // namespace vd::game
