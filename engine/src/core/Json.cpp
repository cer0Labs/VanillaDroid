#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vd::json {

namespace {

constexpr u32 kMaxDepth = 64;
const Value kNull;

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    Result<Value> document() {
        Value value;
        if (Result<void> parsed = parseValue(value, 0); !parsed) {
            return parsed.error();
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            return fail("unexpected text after the value");
        }
        return value;
    }

private:
    Error fail(const char* what) const {
        return Error{"JSON: " + std::string(what) + " at byte " + std::to_string(m_pos)};
    }

    void skipSpace() {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
                                         m_text[m_pos] == '\n' || m_text[m_pos] == '\r')) {
            ++m_pos;
        }
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return false;
        }
        m_pos += word.size();
        return true;
    }

    Result<void> parseValue(Value& out, u32 depth) {
        if (depth > kMaxDepth) {
            return fail("too deep");
        }
        skipSpace();
        if (m_pos >= m_text.size()) {
            return fail("value expected");
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            return parseObject(out, depth);
        }
        if (c == '[') {
            return parseArray(out, depth);
        }
        if (c == '"') {
            std::string text;
            VD_TRY(parseString(text));
            out = Value::string(std::move(text));
            return {};
        }
        if (literal("true")) {
            out = Value::boolean(true);
            return {};
        }
        if (literal("false")) {
            out = Value::boolean(false);
            return {};
        }
        if (literal("null")) {
            out = Value();
            return {};
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(out);
        }
        return fail("unexpected character");
    }

    Result<void> parseObject(Value& out, u32 depth) {
        out = Value::object();
        ++m_pos; // '{'
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return {};
        }
        for (;;) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return fail("key expected");
            }
            std::string key;
            VD_TRY(parseString(key));
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return fail("':' expected");
            }
            ++m_pos;
            Value value;
            VD_TRY(parseValue(value, depth + 1));
            out.set(key, std::move(value));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return {};
            }
            return fail("',' or '}' expected");
        }
    }

    Result<void> parseArray(Value& out, u32 depth) {
        out = Value::array();
        ++m_pos; // '['
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return {};
        }
        for (;;) {
            Value value;
            VD_TRY(parseValue(value, depth + 1));
            out.push(std::move(value));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return {};
            }
            return fail("',' or ']' expected");
        }
    }

    Result<void> parseNumber(Value& out) {
        const usize start = m_pos;
        if (m_text[m_pos] == '-') {
            ++m_pos;
        }
        auto digits = [&] {
            const usize from = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            return m_pos - from;
        };
        if (digits() == 0) {
            return fail("digit expected");
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            if (digits() == 0) {
                return fail("digit expected after '.'");
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            if (digits() == 0) {
                return fail("digit expected in the exponent");
            }
        }
        // strtod needs a terminated string; the program never changes the C locale, so '.' is the separator.
        const std::string token(m_text.substr(start, m_pos - start));
        out = Value::number(std::strtod(token.c_str(), nullptr));
        return {};
    }

    static void appendUtf8(std::string& out, u32 cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(u32& out) {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        out = 0;
        for (usize i = 0; i < 4; ++i) {
            const char c = m_text[m_pos + i];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<u32>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        m_pos += 4;
        return true;
    }

    Result<void> parseString(std::string& out) {
        ++m_pos; // '"'
        for (;;) {
            if (m_pos >= m_text.size()) {
                return fail("unterminated string");
            }
            const char c = m_text[m_pos++];
            if (c == '"') {
                return {};
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                return fail("unterminated escape");
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                u32 cp = 0;
                if (!hex4(cp)) {
                    return fail("bad \\u escape");
                }
                if (cp >= 0xD800 && cp < 0xDC00) { // a surrogate pair: the low half must follow
                    u32 low = 0;
                    if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low >= 0xE000) {
                        return fail("unpaired surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
    }

    std::string_view m_text;
    usize m_pos = 0;
};

void writeString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double number) {
    char text[32];
    if (!std::isfinite(number)) {
        out += "null"; // JSON has no NaN or infinity
        return;
    }
    if (number == std::floor(number) && std::fabs(number) < 9007199254740992.0) {
        std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(number));
    } else {
        std::snprintf(text, sizeof(text), "%.17g", number);
    }
    out += text;
}

void writeValue(std::string& out, const Value& value, bool pretty, u32 indent) {
    auto newline = [&](u32 level) {
        if (pretty) {
            out += '\n';
            out.append(static_cast<usize>(level) * 2, ' ');
        }
    };
    switch (value.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += value.asBool() ? "true" : "false"; break;
    case Type::Number: writeNumber(out, value.asNumber()); break;
    case Type::String: writeString(out, value.asString()); break;
    case Type::Array: {
        if (value.items().empty()) {
            out += "[]";
            break;
        }
        out += '[';
        bool first = true;
        for (const Value& item : value.items()) {
            if (!first) {
                out += ',';
            }
            first = false;
            newline(indent + 1);
            writeValue(out, item, pretty, indent + 1);
        }
        newline(indent);
        out += ']';
        break;
    }
    case Type::Object: {
        if (value.members().empty()) {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (const Member& member : value.members()) {
            if (!first) {
                out += ',';
            }
            first = false;
            newline(indent + 1);
            writeString(out, member.key);
            out += pretty ? ": " : ":";
            writeValue(out, member.value, pretty, indent + 1);
        }
        newline(indent);
        out += '}';
        break;
    }
    }
}

} // namespace

Value Value::boolean(bool value) {
    Value v;
    v.m_type = Type::Bool;
    v.m_bool = value;
    return v;
}

Value Value::number(double value) {
    Value v;
    v.m_type = Type::Number;
    v.m_number = value;
    return v;
}

Value Value::string(std::string value) {
    Value v;
    v.m_type = Type::String;
    v.m_string = std::move(value);
    return v;
}

Value Value::array() {
    Value v;
    v.m_type = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.m_type = Type::Object;
    return v;
}

i64 Value::asInt(i64 fallback) const {
    return m_type == Type::Number ? static_cast<i64>(std::llround(m_number)) : fallback;
}

std::string_view Value::asString(std::string_view fallback) const {
    return m_type == Type::String ? std::string_view(m_string) : fallback;
}

usize Value::size() const {
    return m_type == Type::Array ? m_items.size() : m_type == Type::Object ? m_members.size() : 0;
}

Value& Value::push(Value value) {
    VD_ASSERT(m_type == Type::Array);
    m_items.push_back(std::move(value));
    return m_items.back();
}

const Value* Value::find(std::string_view key) const {
    if (m_type != Type::Object) {
        return nullptr;
    }
    for (const Member& member : m_members) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

const Value& Value::get(std::string_view key) const {
    const Value* value = find(key);
    return value ? *value : kNull;
}

Value& Value::set(std::string_view key, Value value) {
    VD_ASSERT(m_type == Type::Object);
    for (Member& member : m_members) {
        if (member.key == key) {
            member.value = std::move(value);
            return member.value;
        }
    }
    m_members.push_back(Member{std::string(key), std::move(value)});
    return m_members.back().value;
}

bool Value::remove(std::string_view key) {
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->key == key) {
            m_members.erase(it);
            return true;
        }
    }
    return false;
}

Result<Value> parse(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3); // a UTF-8 byte order mark (files written by Windows tools)
    }
    return Parser(text).document();
}

std::string write(const Value& value, bool pretty) {
    std::string out;
    writeValue(out, value, pretty, 0);
    if (pretty) {
        out += '\n';
    }
    return out;
}

} // namespace vd::json
