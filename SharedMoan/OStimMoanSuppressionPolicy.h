#pragma once
#include <cstdint>

namespace TMS::DD::OStimMoanSuppressionPolicy {


    [[nodiscard]] constexpr bool ShouldAttempt(
        bool testMode, std::uint32_t configuredActor, std::uint32_t actor,
        bool ordinaryReaction, bool ignoreChecks, bool cooldownAvailable,
        std::int32_t timerBefore) noexcept
    {
        return testMode && configuredActor != 0 && configuredActor != 0x14u &&
            actor == configuredActor && ordinaryReaction && !ignoreChecks &&
            cooldownAvailable && timerBefore <= 0;
    }



    [[nodiscard]] constexpr bool CooldownRearmed(std::int32_t timerAfter) noexcept
    {
        return timerAfter > 0;
    }
}
