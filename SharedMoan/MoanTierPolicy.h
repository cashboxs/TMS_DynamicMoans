#pragma once

#include <array>
#include <cstdint>
namespace TMS::DD::MoanTierPolicy {
    enum class Tier : std::uint8_t { Low=0, Medium=1, High=2 };
    [[nodiscard]] constexpr const char* Name(Tier tier) noexcept {
        switch (tier) { case Tier::Low: return "Low"; case Tier::Medium: return "Medium"; case Tier::High: return "High"; }
        return "Unknown";
    }
    [[nodiscard]] constexpr int Classify(int excitement, int lowMax, int mediumMax) noexcept {
        if(excitement < 0 || excitement > 100 || lowMax < 0 || lowMax >= mediumMax || mediumMax >= 100)
            return -1;
        return excitement <= lowMax ? 0 : (excitement <= mediumMax ? 1 : 2);
    }


    [[nodiscard]] constexpr int CreatureBoost(int currentTier, bool creature) noexcept {
        if (currentTier < 0 || currentTier > 2) return currentTier;
        return creature && currentTier < 2 ? currentTier + 1 : currentTier;
    }


    [[nodiscard]] constexpr int Fallback(int wanted, const std::array<unsigned,3>& counts) noexcept {
        if(wanted < 0 || wanted > 2) return -1;
        if(counts[wanted]) return wanted;
        for(int distance=1; distance<=2; ++distance) {
            if(wanted-distance >= 0 && counts[wanted-distance]) return wanted-distance;
            if(wanted+distance <= 2 && counts[wanted+distance]) return wanted+distance;
        }
        return -1;
    }
}
