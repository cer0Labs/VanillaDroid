#pragma once

#include "core/Types.h"

namespace vd {

// Named threads show up readably in Perfetto, AGI, Tracy and debuggers (Android: max 15 chars).
void setCurrentThreadName(const char* name);

// Kernel id of the calling thread (Android: gettid), for ADPF sessions and affinity; 0 elsewhere.
i32 currentThreadId();

// Keeps thread `tid` off the slowest CPU cluster (Android; a no-op elsewhere). Returns the number of cores it may
// use, 0 if nothing changed (a single cluster, or not permitted).
u32 pinThreadToFastCores(i32 tid);

} // namespace vd
