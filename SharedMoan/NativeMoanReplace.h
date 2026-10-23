#pragma once
#include <cstdint>
#include <string>

namespace RE { class Actor; class BSAudioManager; struct BSSoundHandle; }
namespace TMS::DD::NativeMoanReplace {

    void LoadSettings();
    bool IsEnabled() noexcept;
#if !defined(TMS_MOAN_STANDALONE)
    void __stdcall RenderFrequencyPage();
#endif
    void SetNativeCallHookInstalled(bool value) noexcept;
    bool IsReady() noexcept;
    bool IsClimaxEnabled() noexcept;
    bool IsClimaxHookAvailable() noexcept;
    void SetClimaxHookInstalled(bool ready) noexcept;
    void SetClimaxEnabled(bool value) noexcept;
    bool ShouldSuppressForVoiceGuard(std::uint32_t actorFormID) noexcept;
    class OrdinaryScope {
    public:

        OrdinaryScope(std::uint32_t actorFormID, RE::Actor* actor, RE::Actor* partnerActor = nullptr) noexcept;
        ~OrdinaryScope() noexcept;
        OrdinaryScope(const OrdinaryScope&) = delete;
        OrdinaryScope& operator=(const OrdinaryScope&) = delete;

        void AdjustNativeCooldown(void* timerAddress) const noexcept;
    private:
        bool previousActive_ = false;
        bool previousConsumed_ = false;
        bool previousReplaced_ = false;
        const void* previousFile_ = nullptr;
        bool selected_ = false;
        int excitement_ = -1;
        int baseTier_ = -1;
        float ppaFrequencyMultiplier_ = 1.0f;
        bool ppaApplied_ = false;
        int tierRequested_ = -1;
        int tierUsed_ = -1;
        bool fallback_ = false;
#if defined(TMS_MOAN_STANDALONE)
        bool maleLibrary_ = false;
        bool creaturePartner_ = false;
        bool creatureBoostApplied_ = false;
#endif
        std::uint32_t actorFormID_ = 0;
        const char* reason_ = "NoSelection";
    };


    bool HasClimaxFiles() noexcept;
    class ClimaxScope {
    public:
        explicit ClimaxScope(std::uint32_t actorFormID) noexcept;
        ~ClimaxScope() noexcept;
        bool Suppressed() const noexcept { return suppressed_; }
        ClimaxScope(const ClimaxScope&) = delete;
        ClimaxScope& operator=(const ClimaxScope&) = delete;
    private:
        bool previousActive_=false,previousConsumed_=false,previousReplaced_=false;
        const void* previousFile_=nullptr;
        std::uint32_t actorFormID_=0;
        bool selected_=false,suppressed_=false;
        const char* reason_="NotSelected";
    };
    using NativeBuildFn = void (*)(RE::BSAudioManager*, RE::BSSoundHandle*, void*, std::uint32_t);
    void SetOriginalBuildFn(NativeBuildFn fn) noexcept;
    void __cdecl OnNativeBuildSound(RE::BSAudioManager* mgr,
        RE::BSSoundHandle* handle, void* descriptorData, std::uint32_t flags) noexcept;
}
