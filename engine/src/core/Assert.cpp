#include "core/Assert.h"

#include "core/Log.h"

#include <cstdlib>

namespace vd::detail {

void assertFailed(const char* expression, const char* file, int line) {
    VD_LOG_ERROR("Assertion failed: %s (%s:%d)", expression, file, line);
    std::abort();
}

} // namespace vd::detail
