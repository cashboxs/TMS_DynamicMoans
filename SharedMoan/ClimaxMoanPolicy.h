#pragma once
#include <cstdint>
namespace TMS::DD::ClimaxMoanPolicy {
    enum class Decision : std::uint8_t { Pass, Replace, Suppress };


    constexpr Decision Decide(bool hookReady, bool wavsAvailable,
        bool enabled, bool actorMatched, bool guarded) noexcept {
        if (!hookReady || !wavsAvailable || !enabled || !actorMatched)
            return Decision::Pass;
        return guarded ? Decision::Suppress : Decision::Replace;
    }
}
