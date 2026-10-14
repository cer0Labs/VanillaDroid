#pragma once

#include "core/Assert.h"

#include <optional>
#include <string>
#include <utility>

namespace vd {

// Error path may allocate; the success path must not.
struct Error {
    std::string message;
};

// Minimal expected-like type (std::expected is not available on the VS 2019 toolchain).
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    Result(Error error) : m_error(std::move(error)) {}

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }

    T& value() {
        VD_ASSERT(ok());
        return *m_value;
    }
    const T& value() const {
        VD_ASSERT(ok());
        return *m_value;
    }
    const Error& error() const {
        VD_ASSERT(!ok());
        return m_error;
    }

private:
    std::optional<T> m_value;
    Error m_error;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}

    bool ok() const { return m_ok; }
    explicit operator bool() const { return ok(); }

    const Error& error() const {
        VD_ASSERT(!ok());
        return m_error;
    }

private:
    Error m_error;
    bool m_ok = true;
};

} // namespace vd

// Propagates the error of a Result-returning expression from the enclosing function.
#define VD_TRY(expr)                                                                                     \
    do {                                                                                                 \
        auto vdTryResult_ = (expr);                                                                      \
        if (!vdTryResult_.ok()) {                                                                        \
            return vdTryResult_.error();                                                                 \
        }                                                                                                \
    } while (0)
