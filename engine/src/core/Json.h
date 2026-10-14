#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (distribution): a small JSON document for the files that travel between the baker, VanillaDroid
// Setup, the phone and the public releases (packs/manifest.json, release/packs.json, files.json, the release
// manifest). Not for hot paths: values own their strings and children. Objects keep their keys in insertion
// order, so a file written twice from the same data is the same bytes.
namespace vd::json {

enum class Type : u8 { Null, Bool, Number, String, Array, Object };

struct Member;

class Value {
public:
    Value() = default; // null
    static Value boolean(bool value);
    static Value number(double value);
    static Value string(std::string value);
    static Value array();
    static Value object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    // The value, or the fallback when it has another type.
    bool asBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    double asNumber(double fallback = 0.0) const { return m_type == Type::Number ? m_number : fallback; }
    i64 asInt(i64 fallback = 0) const;
    std::string_view asString(std::string_view fallback = {}) const;

    // Arrays.
    std::span<const Value> items() const { return m_items; }
    usize size() const; // items of an array, members of an object
    Value& push(Value value);

    // Objects. get() returns a null value for a missing key (or when this is not an object), so lookups
    // chain: doc.get("client").get("locale").asString().
    std::span<const Member> members() const;
    const Value* find(std::string_view key) const;
    const Value& get(std::string_view key) const;
    Value& set(std::string_view key, Value value); // replaces the key's value if it is there
    bool remove(std::string_view key);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<Member> m_members;
};

struct Member {
    std::string key;
    Value value;
};

inline std::span<const Member> Value::members() const {
    return m_members;
}

// RFC 8259 text (UTF-8, no comments, no trailing commas); at most 64 levels deep.
Result<Value> parse(std::string_view text);
// Two-space indentation with `pretty`, else one line. Whole numbers below 2^53 have no decimals.
std::string write(const Value& value, bool pretty = true);

} // namespace vd::json
