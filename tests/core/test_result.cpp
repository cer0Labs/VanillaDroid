#include "core/Result.h"

#include <doctest/doctest.h>

namespace {

vd::Result<int> parsePositive(int value) {
    if (value <= 0) {
        return vd::Error{"not positive"};
    }
    return value;
}

vd::Result<void> checkEven(int value) {
    if (value % 2 != 0) {
        return vd::Error{"odd"};
    }
    return {};
}

} // namespace

TEST_CASE("Result<T> carries either a value or an error") {
    const vd::Result<int> good = parsePositive(5);
    REQUIRE(good.ok());
    CHECK(good.value() == 5);

    const vd::Result<int> bad = parsePositive(-1);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "not positive");
}

TEST_CASE("Result<void> signals success or error") {
    CHECK(checkEven(4).ok());

    const vd::Result<void> bad = checkEven(3);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "odd");
}
