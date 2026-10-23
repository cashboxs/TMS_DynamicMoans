#pragma once



#include "MoanPhysicalPolicy.h"
#include <cmath>
#include <cstdint>

namespace TMS::DD::PPAContactAggregation {


template<class Range>
[[nodiscard]] bool HasDualReceivingSites(const Range& partners) noexcept {
    using Site=MoanPhysicalPolicy::Site;
    for(const auto& vaginal : partners) {
        if(!vaginal.partner || vaginal.site!=Site::Vagina ||
           !std::isfinite(vaginal.depth) || vaginal.depth<=0.0f) continue;
        for(const auto& anal : partners) {
            if(!anal.partner || anal.partner==vaginal.partner || anal.site!=Site::Anus ||
               !std::isfinite(anal.depth) || anal.depth<=0.0f) continue;
            return true;
        }
    }
    return false;
}
}
