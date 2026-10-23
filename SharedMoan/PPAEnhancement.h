#pragma once
#include <cstdint>
#include "MoanPhysicalPolicy.h"
namespace TMS::DD::PPAEnhancement {
struct Decision {
    bool valid=false;
    bool applied=false;
    MoanPhysicalPolicy::Role role=MoanPhysicalPolicy::Role::None;
    MoanPhysicalPolicy::Site site=MoanPhysicalPolicy::Site::None;
    int ageMs=0;
    int score=0, floor=0;
    float multiplier=1.0f;
};
void LoadSettings();
void Connect() noexcept;
void ResetSnapshots() noexcept;
bool IsConnected() noexcept;


bool IsReceivingMouthContact(std::uint32_t actorFormID,int* ageMs=nullptr) noexcept;
Decision EvaluateActor(std::uint32_t actorFormID,int excitement) noexcept;
void __stdcall RenderMenu();
}
