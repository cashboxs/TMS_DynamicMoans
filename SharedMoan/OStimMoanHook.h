#pragma once
#include <cstdint>
namespace TMS::DD::OStimMoanHook {



    void TryInstallAutoHook(bool enabled, bool suppressTestMode,
        std::uint32_t testActorFormID);
}
