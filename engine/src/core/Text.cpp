#include "core/Text.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "pack/AppPack.h"
#include "pack/PackReader.h"
#include "pack/UiFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vd::text {
namespace {

// One table blob as the baker writes it (pack::UiStringsHeader), read in place.
struct Source {
    const pack::UiString* entries = nullptr;
    u32 count = 0;
    const char* text = nullptr; // the texts block
    u32 textSize = 0;
};

// A text of the merged table (phase 22): its key's hash and where its bytes are (in a source's texts block).
struct Entry {
    u64 keyHash;
    const char* text;
    u32 length;
};

struct Table {
    pack::PackReader uiPack;  // the clients' tables
    pack::PackReader appPack; // ours
    std::vector<Entry> entries; // sorted by keyHash
    std::string locale;
};

Table& table() {
    static Table instance;
    return instance;
}

// Keys reported missing, so each is warned about once (no allocation: a small ring of hashes).
std::array<std::atomic<u64>, 64> g_missing{};
std::atomic<u32> g_missingNext{0};

void warnMissing(const Key& key) {
    for (const std::atomic<u64>& seen : g_missing) {
        if (seen.load(std::memory_order_relaxed) == key.hash) {
            return;
        }
    }
    g_missing[g_missingNext.fetch_add(1, std::memory_order_relaxed) % g_missing.size()].store(
        key.hash, std::memory_order_relaxed);
    VD_LOG_WARN("Text %.*s is missing from the strings table", static_cast<int>(key.name.size()), key.name.data());
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

bool parseSource(std::span<const u8> blob, Source& out) {
    pack::UiStringsHeader header{};
    if (blob.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, blob.data(), sizeof(header));
    const u64 entriesEnd = sizeof(header) + u64{header.count} * sizeof(pack::UiString);
    if (entriesEnd > blob.size() || u64{header.textOffset} + header.textSize > blob.size() ||
        header.textOffset < entriesEnd) {
        return false;
    }
    out.entries = reinterpret_cast<const pack::UiString*>(blob.data() + sizeof(header));
    out.count = header.count;
    out.text = reinterpret_cast<const char*>(blob.data() + header.textOffset);
    out.textSize = header.textSize;
    return true;
}

// The tables in priority order (the first one with a key gives its text), each sorted by hash, into one sorted table:
// a walk over all of them at once (allocates: at load only).
void merge(std::span<const Source> sources, std::vector<Entry>& out) {
    out.clear();
    usize total = 0;
    for (const Source& s : sources) {
        total += s.count;
    }
    out.reserve(total);
    std::array<u32, 8> next{};
    const usize count = std::min(sources.size(), next.size());
    for (;;) {
        u64 smallest = 0;
        bool any = false;
        for (usize i = 0; i < count; ++i) {
            if (next[i] < sources[i].count && (!any || sources[i].entries[next[i]].keyHash < smallest)) {
                smallest = sources[i].entries[next[i]].keyHash;
                any = true;
            }
        }
        if (!any) {
            return;
        }
        bool taken = false;
        for (usize i = 0; i < count; ++i) {
            const Source& s = sources[i];
            while (next[i] < s.count && s.entries[next[i]].keyHash == smallest) {
                const pack::UiString& e = s.entries[next[i]++];
                if (!taken && u64{e.offset} + e.length < s.textSize) {
                    out.push_back({smallest, s.text + e.offset, e.length});
                    taken = true;
                }
            }
        }
    }
}

// The lookup order of a language (Text.h): its client's table unless it is English, ours, our English, the base
// client's.
void mergeTables(const Tables& tables) {
    const bool english = lowerAscii(tables.locale) == "enus";
    std::array<Source, 5> sources{};
    usize count = 0;
    auto add = [&](std::span<const u8> blob) {
        if (!blob.empty() && parseSource(blob, sources[count])) {
            ++count;
        } else if (!blob.empty()) {
            VD_LOG_ERROR("Texts: malformed strings table");
        }
    };
    if (!english) {
        add(tables.client); // a client's own wording beats our translations of it
    }
    add(tables.own);
    add(tables.ownEnglish);
    if (english) {
        add(tables.client); // our English texts fix the client's
    }
    add(tables.clientBase);
    merge({sources.data(), count}, table().entries);
}

const Entry* find(u64 hash) {
    const std::vector<Entry>& entries = table().entries;
    const auto it = std::lower_bound(entries.begin(), entries.end(), hash,
                                     [](const Entry& e, u64 h) { return e.keyHash < h; });
    return it != entries.end() && it->keyHash == hash ? &*it : nullptr;
}

// Output with truncation: keeps room for the NUL.
struct Writer {
    std::span<char> out;
    usize used = 0;

    void put(std::string_view s) {
        if (out.empty()) {
            return;
        }
        usize n = std::min(s.size(), out.size() - 1 - used);
        // Cut before a character, not inside one (phase 21: translations are longer and carry á, ñ...).
        while (n < s.size() && n > 0 && (static_cast<u8>(s[n]) & 0xC0) == 0x80) {
            --n;
        }
        std::memcpy(out.data() + used, s.data(), n);
        used += n;
    }
    void put(char c) { put(std::string_view(&c, 1)); }
    std::string_view finish() {
        if (out.empty()) {
            return {};
        }
        out[used] = 0;
        return {out.data(), used};
    }
};

// One conversion of a format, already parsed.
struct Spec {
    char flags[6] = {};
    u32 flagCount = 0;
    i32 width = -1;
    i32 precision = -1;
    char conversion = 's';
};

// Prints one argument as `spec` says, building the single-conversion printf format here (never from data).
void printArg(Writer& w, const Spec& spec, const Arg* arg) {
    if (!arg) {
        return;
    }
    char format[24];
    usize n = 0;
    format[n++] = '%';
    for (u32 i = 0; i < spec.flagCount; ++i) {
        format[n++] = spec.flags[i];
    }
    auto number = [&](i32 value) {
        n += static_cast<usize>(std::snprintf(format + n, sizeof(format) - n, "%d", std::min(value, 99)));
    };
    if (spec.width >= 0) {
        number(spec.width);
    }
    if (spec.precision >= 0) {
        format[n++] = '.';
        number(spec.precision);
    }
    char buffer[128];
    int written = 0;
    char conversion = spec.conversion;
    // What the argument is decides how it prints; the specifier only shapes it.
    switch (arg->kind) {
    case Arg::Kind::Str:
        if (conversion != 's') {
            w.put(arg->s);
            return;
        }
        format[n++] = 's';
        format[n] = 0;
        if (spec.width < 0 && spec.precision < 0) {
            w.put(arg->s);
            return;
        }
        {
            const std::string_view s = arg->s.substr(0, std::min<usize>(arg->s.size(), 100));
            char copy[104];
            std::memcpy(copy, s.data(), s.size());
            copy[s.size()] = 0;
            written = std::snprintf(buffer, sizeof(buffer), format, copy);
        }
        break;
    case Arg::Kind::Char:
        format[n++] = 'c';
        format[n] = 0;
        written = std::snprintf(buffer, sizeof(buffer), format, static_cast<int>(arg->i));
        break;
    case Arg::Kind::Int:
    case Arg::Kind::Uint: {
        const bool isSigned = arg->kind == Arg::Kind::Int;
        if (conversion == 'f' || conversion == 'g' || conversion == 'e') {
            format[n++] = conversion;
            format[n] = 0;
            written = std::snprintf(buffer, sizeof(buffer), format,
                                    isSigned ? static_cast<f64>(arg->i) : static_cast<f64>(arg->u));
            break;
        }
        format[n++] = 'l';
        format[n++] = 'l';
        format[n++] = conversion == 'x' || conversion == 'X' ? conversion : (isSigned ? 'd' : 'u');
        format[n] = 0;
        written = isSigned ? std::snprintf(buffer, sizeof(buffer), format, static_cast<long long>(arg->i))
                           : std::snprintf(buffer, sizeof(buffer), format, static_cast<unsigned long long>(arg->u));
        break;
    }
    case Arg::Kind::Float:
        if (conversion == 'd' || conversion == 'i' || conversion == 'u') {
            format[n++] = 'l';
            format[n++] = 'l';
            format[n++] = 'd';
            format[n] = 0;
            written = std::snprintf(buffer, sizeof(buffer), format, static_cast<long long>(arg->f));
        } else {
            format[n++] = conversion == 'g' || conversion == 'e' ? conversion : 'f';
            format[n] = 0;
            written = std::snprintf(buffer, sizeof(buffer), format, arg->f);
        }
        break;
    }
    if (written > 0) {
        w.put(std::string_view(buffer, std::min<usize>(static_cast<usize>(written), sizeof(buffer) - 1)));
    }
}

std::atomic<u8> g_gender{0}; // phase 21: setGender

void formatInto(Writer& w, std::string_view format, std::span<const Arg> args) {
    usize next = 0; // the next sequential argument
    for (usize i = 0; i < format.size();) {
        const char c = format[i];
        // Phase 21: "$g<male>:<female>;" by the player's gender (the esES ABANDON_QUEST_CONFIRM_WITH_ITEMS: "¿Estás
        // $gseguro:segura;?"), as the quest texts word it.
        if (c == '$' && i + 1 < format.size() && (format[i + 1] == 'g' || format[i + 1] == 'G')) {
            const usize colon = format.find(':', i + 2);
            const usize end = colon == std::string_view::npos ? colon : format.find(';', colon + 1);
            if (end != std::string_view::npos) {
                w.put(g_gender.load(std::memory_order_relaxed) == 1 ? format.substr(colon + 1, end - colon - 1)
                                                                     : format.substr(i + 2, colon - i - 2));
                i = end + 1;
                continue;
            }
        }
        if (c != '%') {
            const usize end = std::min(format.find_first_of("%$", i + 1), format.size());
            w.put(format.substr(i, end - i));
            i = end;
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == '%') {
            w.put('%');
            i += 2;
            continue;
        }
        // %[N$][flags][width][.precision][length]conversion
        usize j = i + 1;
        Spec spec;
        usize position = 0; // 1-based, 0 = sequential
        usize digitsEnd = j;
        while (digitsEnd < format.size() && format[digitsEnd] >= '0' && format[digitsEnd] <= '9') {
            ++digitsEnd;
        }
        if (digitsEnd > j && digitsEnd < format.size() && format[digitsEnd] == '$') {
            for (usize k = j; k < digitsEnd; ++k) {
                position = position * 10 + static_cast<usize>(format[k] - '0');
            }
            j = digitsEnd + 1;
        }
        while (j < format.size() && std::strchr("-+ #0", format[j]) && spec.flagCount < 5) {
            spec.flags[spec.flagCount++] = format[j++];
        }
        if (j < format.size() && format[j] >= '0' && format[j] <= '9') {
            spec.width = 0;
            while (j < format.size() && format[j] >= '0' && format[j] <= '9') {
                spec.width = std::min(spec.width * 10 + (format[j++] - '0'), 99);
            }
        }
        if (j < format.size() && format[j] == '.') {
            ++j;
            spec.precision = 0;
            while (j < format.size() && format[j] >= '0' && format[j] <= '9') {
                spec.precision = std::min(spec.precision * 10 + (format[j++] - '0'), 99);
            }
        }
        while (j < format.size() && (format[j] == 'l' || format[j] == 'h')) {
            ++j;
        }
        if (j >= format.size() || !std::strchr("sdiucfgexX", format[j])) {
            w.put(format.substr(i, j - i)); // not a conversion: printed as is
            i = j;
            continue;
        }
        spec.conversion = format[j];
        const usize index = position ? position - 1 : next++;
        printArg(w, spec, index < args.size() ? &args[index] : nullptr);
        i = j + 1;
    }
}

} // namespace

bool load(const std::filesystem::path& uiPack, const std::filesystem::path& appPack, std::string_view locale) {
    const Clock::time_point start = Clock::now();
    Table& t = table();
    unload();
    if (!uiPack.empty()) {
        if (Result<void> opened = t.uiPack.open(uiPack); !opened) {
            VD_LOG_WARN("Texts: %s", opened.error().message.c_str());
        }
    }
    if (!appPack.empty()) {
        if (Result<void> opened = t.appPack.open(appPack); !opened) {
            VD_LOG_WARN("Texts: %s (our texts: vd_baker bake --out packs --only app)", opened.error().message.c_str());
        }
    }
    // A table of a pack, mapped in place (empty when the pack lacks it).
    const auto blob = [](const pack::PackReader& source, const std::string& name) -> std::span<const u8> {
        const std::optional<pack::PackAsset> asset = source.find(pack::kUiStringsPrefix + name);
        if (!asset || asset->compression != pack::Compression::None) {
            return {};
        }
        return {asset->data, asset->size};
    };
    const std::string wanted = lowerAscii(locale);
    // The base client's language is ui.vdpk's (older packs did not say it: English).
    std::string base = lowerAscii(t.uiPack.locale());
    if (base.empty()) {
        base = "enus";
    }
    Tables tables;
    tables.locale = wanted;
    tables.client = blob(t.uiPack, wanted);
    tables.clientBase = blob(t.uiPack, base);
    tables.own = blob(t.appPack, wanted);
    tables.ownEnglish = blob(t.appPack, "enus");
    mergeTables(tables);
    if (t.entries.empty()) {
        VD_LOG_WARN("Texts: no strings table in %s or %s (bake them: vd_baker bake --only ui, --only app)",
                    uiPack.string().c_str(), appPack.string().c_str());
        unload();
        return false;
    }
    if (wanted != "enus" && tables.client.empty() && tables.own.empty()) {
        VD_LOG_WARN("Texts: no %.*s table in the packs, using enUS", static_cast<int>(locale.size()), locale.data());
    }
    t.locale = wanted;
    VD_LOG_INFO("Texts: %zu in %s (its client's: %s, the base client's: %s, ours: %s), %.2f ms", t.entries.size(),
                wanted.c_str(), tables.client.empty() ? "no" : "yes", tables.clientBase.empty() ? "no" : base.c_str(),
                !tables.own.empty() ? wanted.c_str() : tables.ownEnglish.empty() ? "none" : "enus",
                secondsBetween(start, Clock::now()) * 1000.0);
    if (tables.ownEnglish.empty()) {
        VD_LOG_WARN("Texts: no table of ours in %s (our VD_* keys show as themselves)", appPack.string().c_str());
    }
    return true;
}

bool load(const std::filesystem::path& uiPack, std::string_view locale) {
    return load(uiPack, uiPack.parent_path() / pack::kAppPackFile, locale);
}

void setTables(const Tables& tables) {
    unload();
    mergeTables(tables);
    table().locale = lowerAscii(tables.locale);
}

void setTable(std::span<const u8> blob) {
    unload();
    Source source;
    if (!parseSource(blob, source)) {
        VD_LOG_ERROR("Texts: malformed strings table");
        return;
    }
    merge({&source, 1}, table().entries);
}

void unload() {
    Table& t = table();
    t.entries.clear();
    t.locale.clear();
    t.uiPack.close();
    t.appPack.close();
}

bool loaded() {
    return !table().entries.empty();
}

void setGender(u8 gender) {
    g_gender.store(gender, std::memory_order_relaxed);
}

bool has(Key key) {
    return find(key.hash) != nullptr;
}

std::string_view get(Key key) {
    if (const Entry* e = find(key.hash)) {
        return {e->text, e->length};
    }
    warnMissing(key);
    return key.name;
}

std::string_view vformat(std::span<char> out, std::string_view format, std::span<const Arg> args) {
    Writer w{out};
    formatInto(w, format, args);
    return w.finish();
}

void vappend(std::string& out, std::string_view format, std::span<const Arg> args) {
    char buffer[1024];
    out += vformat(buffer, format, args);
}

} // namespace vd::text
