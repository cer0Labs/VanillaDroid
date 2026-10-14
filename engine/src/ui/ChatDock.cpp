#include "ui/ChatDock.h"

#include <algorithm>
#include <array>

namespace vd::ui {

f32 chatLineAlpha(u64 shownMs, u64 nowMs, u32 visibleMs, u32 fadeMs) {
    if (nowMs <= shownMs + visibleMs) {
        return 1.0f;
    }
    const u64 fading = nowMs - shownMs - visibleMs;
    return fading >= fadeMs ? 0.0f : 1.0f - static_cast<f32>(fading) / static_cast<f32>(fadeMs);
}

bool flashOver(u64 elapsedMs, const FlashTiming& timing) {
    return elapsedMs > timing.durationMs;
}

f32 flashAlpha(u64 elapsedMs, const FlashTiming& timing) {
    if (flashOver(elapsedMs, timing)) {
        return 0.0f;
    }
    const u64 cycle = u64{timing.fadeInMs} + timing.inHoldMs + timing.fadeOutMs + timing.outHoldMs;
    if (cycle == 0) {
        return 1.0f;
    }
    u64 t = elapsedMs % cycle;
    if (t < timing.fadeInMs) {
        return static_cast<f32>(t) / static_cast<f32>(timing.fadeInMs);
    }
    t -= timing.fadeInMs;
    if (t < timing.inHoldMs) {
        return 1.0f;
    }
    t -= timing.inHoldMs;
    if (t < timing.fadeOutMs) {
        return 1.0f - static_cast<f32>(t) / static_cast<f32>(timing.fadeOutMs);
    }
    return 0.0f;
}

bool bottomButtonLit(u64 nowMs) {
    return (nowMs / kChatButtonFlashMs) % 2 == 0;
}

f32 fadeTowards(f32 alpha, f32 target, f32 dtMs, f32 fadeMs) {
    const f32 step = fadeMs > 0.0f ? std::max(dtMs, 0.0f) / fadeMs : 1.0f;
    return alpha < target ? std::min(target, alpha + step) : std::max(target, alpha - step);
}

void dockTabWidths(std::span<f32> widths, f32 available) {
    f32 total = 0.0f;
    for (const f32 w : widths) {
        total += w;
    }
    if (widths.empty() || total <= available) {
        return;
    }
    // From the narrowest: one under the average of what is left keeps its width; from the first that is not,
    // every one left gets that average.
    constexpr usize kMaxTabs = 16;
    std::array<u32, kMaxTabs> order{};
    const u32 count = static_cast<u32>(std::min(widths.size(), kMaxTabs));
    for (u32 i = 0; i < count; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.begin() + count,
                     [&](u32 a, u32 b) { return widths[a] < widths[b]; });
    f32 left = std::max(available, 0.0f);
    for (u32 k = 0; k < count; ++k) {
        const f32 average = left / static_cast<f32>(count - k);
        if (widths[order[k]] < average) {
            left -= widths[order[k]];
            continue;
        }
        for (u32 j = k; j < count; ++j) {
            widths[order[j]] = average;
        }
        break;
    }
}

} // namespace vd::ui
