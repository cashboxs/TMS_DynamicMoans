#pragma once



#include <algorithm>
#include <cstdint>
namespace TMS::DD::MoanPhysicalPolicy {
enum class Role : std::uint8_t { None=0, Receiving=1, Penetrating=2, Self=3 };
enum class Site : std::uint8_t { None=0, Mouth=1, Anus=2, Vagina=3, Both=4, HandL=5, HandR=6, Hands=7 };
[[nodiscard]] constexpr const char* RoleName(Role role) noexcept {
    switch(role) {
    case Role::Receiving:return "Receiving";
    case Role::Penetrating:return "Penetrating";
    case Role::Self:return "Self";
    default:return "None";
    }
}
[[nodiscard]] constexpr const char* SiteName(Site site) noexcept {
    switch(site) {
    case Site::Mouth:return "Mouth";
    case Site::Anus:return "Anus";
    case Site::Vagina:return "Vagina";
    case Site::Both:return "Both";
    case Site::HandL:return "HandL";
    case Site::HandR:return "HandR";
    case Site::Hands:return "Hands";
    default:return "None";
    }
}
[[nodiscard]] constexpr bool IsSupportedSite(Site site) noexcept {
    return site==Site::Vagina || site==Site::Anus || site==Site::Both;
}
struct Input {
    int excitement=0;
    Role role=Role::None;
    Site site=Site::None;
};
struct Result {
    int score=0;
    float intervalMultiplier=1.0f;
    int floor=0;
};
[[nodiscard]] constexpr Result Evaluate(const Input& in) noexcept {
    Result out;
    out.score=std::clamp(in.excitement,0,100);
    if(!IsSupportedSite(in.site)) return out;
    const bool receiving=in.role==Role::Receiving || in.role==Role::Self;
    if(receiving) {
        if(in.site==Site::Anus) out.floor=34;
        if(in.site==Site::Both) out.floor=67;
        switch(in.site) {
        case Site::Anus: out.intervalMultiplier=0.80f;break;
        case Site::Both: out.intervalMultiplier=0.65f;break;
        default: break;
        }
    } else if(in.role==Role::Penetrating) {
        switch(in.site) {
        case Site::Vagina: out.intervalMultiplier=0.90f;break;
        case Site::Anus: out.intervalMultiplier=0.85f;break;
        case Site::Both: out.intervalMultiplier=0.80f;break;
        default: break;
        }
    }
    out.score=std::max(out.score,out.floor);
    return out;
}
}
