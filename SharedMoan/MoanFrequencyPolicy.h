#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
namespace TMS::DD::MoanFrequencyPolicy {
    [[nodiscard]] inline std::int32_t NextCooldownMs(float intervalSeconds,float durationSeconds,float gapSeconds) noexcept {
        if (!std::isfinite(intervalSeconds) || !std::isfinite(durationSeconds) || !std::isfinite(gapSeconds) ||
            intervalSeconds < 1.0f || intervalSeconds > 15.0f ||
            durationSeconds <= 0.0f || durationSeconds > 300.0f ||
            gapSeconds < 0.2f || gapSeconds > 2.0f) return 0;
        const float seconds=std::max(intervalSeconds,durationSeconds+gapSeconds);

        return static_cast<std::int32_t>(std::ceil(seconds*1000.0f));
    }
}
