#include "core/Json.h"

#include <doctest/doctest.h>

#include <ostream> // doctest prints std::string values through operator<<
#include <string>

using vd::json::Value;

TEST_CASE("JSON: parse objects, arrays, strings and numbers") {
    const vd::Result<Value> parsed = vd::json::parse(R"({
        "format": 1,
        "name": "VanillaDroid",
        "size": 4501640000,
        "ratio": -0.25e1,
        "ok": true,
        "nothing": null,
        "list": [1, "two", [3], {"four": 4}],
        "escaped": "a\"b\\c\/d\n\u00e9\ud83d\ude00"
    })");
    REQUIRE(parsed.ok());
    const Value& root = parsed.value();
    CHECK(root.isObject());
    CHECK(root.get("format").asInt() == 1);
    CHECK(root.get("name").asString() == "VanillaDroid");
    CHECK(root.get("size").asInt() == 4501640000LL);
    CHECK(root.get("ratio").asNumber() == doctest::Approx(-2.5));
    CHECK(root.get("ok").asBool());
    CHECK(root.get("nothing").isNull());
    CHECK(root.get("missing").isNull());
    CHECK(root.get("missing").get("deeper").asString("fallback") == "fallback");
    const Value& list = root.get("list");
    REQUIRE(list.size() == 4);
    CHECK(list.items()[1].asString() == "two");
    CHECK(list.items()[2].items()[0].asInt() == 3);
    CHECK(list.items()[3].get("four").asInt() == 4);
    CHECK(root.get("escaped").asString() == "a\"b\\c/d\n\xC3\xA9\xF0\x9F\x98\x80");
}

TEST_CASE("JSON: errors") {
    CHECK_FALSE(vd::json::parse("").ok());
    CHECK_FALSE(vd::json::parse("{").ok());
    CHECK_FALSE(vd::json::parse("{\"a\" 1}").ok());
    CHECK_FALSE(vd::json::parse("[1,]").ok());
    CHECK_FALSE(vd::json::parse("{\"a\":1,}").ok());
    CHECK_FALSE(vd::json::parse("\"unterminated").ok());
    CHECK_FALSE(vd::json::parse("\"\\ud83d\"").ok()); // half a surrogate pair
    CHECK_FALSE(vd::json::parse("01x").ok());
    CHECK_FALSE(vd::json::parse("1 2").ok());
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK_FALSE(vd::json::parse(deep).ok());
    CHECK(vd::json::parse("\xEF\xBB\xBF{}").ok()); // a byte order mark is skipped
}

TEST_CASE("JSON: write keeps the key order and reads back the same") {
    Value root = Value::object();
    root.set("format", Value::number(1));
    root.set("size", Value::number(4501640000.0));
    root.set("half", Value::number(0.5));
    root.set("text", Value::string("tab\there \"quoted\" \x01"));
    Value& list = root.set("list", Value::array());
    list.push(Value::boolean(false));
    list.push(Value());
    root.set("empty", Value::object());
    root.set("format", Value::number(2)); // replaced in place, not moved to the end

    // Outside the CHECK: MSVC misreads a raw string holding \" inside a macro argument.
    const std::string expected = R"({"format":2,"size":4501640000,"half":0.5,"text":"tab\there \"quoted\" \u0001",)"
                                 R"("list":[false,null],"empty":{}})";
    const std::string compact = vd::json::write(root, false);
    CHECK(compact == expected);
    const std::string pretty = vd::json::write(root);
    CHECK(pretty.find("{\n  \"format\": 2,\n") == 0);
    const vd::Result<Value> again = vd::json::parse(pretty);
    REQUIRE(again.ok());
    CHECK(vd::json::write(again.value(), false) == compact);

    CHECK(root.remove("half"));
    CHECK_FALSE(root.remove("half"));
    CHECK(root.find("half") == nullptr);
    CHECK(root.size() == 5);
}
