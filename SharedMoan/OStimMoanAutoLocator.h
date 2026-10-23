#pragma once


#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace TMS::DD::OStimMoanAutoLocator {
    struct ScanResult {
        bool validated = false;
        std::uint32_t candidateCount = 0;
        std::uint32_t validatedCount = 0;
        std::uint32_t loopRva = 0;
        std::uint32_t hookRva = 0;
        std::uint32_t playRva = 0;
        std::uint32_t cooldownRva = 0;



        std::uint32_t loopEndRva = 0;
        std::uint32_t playEndRva = 0;
        std::uint32_t cooldownEndRva = 0;
        std::uint32_t actorOffset = 0;
        std::uint32_t partnerOffset = 0;
        std::uint32_t moanOffset = 0;
        std::uint32_t cooldownOffset = 0;
        std::string reason;
    };




    struct SoundBuildCallResult {
        bool validated = false;
        std::uint32_t candidateCount = 0;
        std::uint32_t validCount = 0;
        std::uint32_t gameSoundPlayRva = 0;
        std::uint32_t gameSoundPlayEndRva = 0;
        std::uint32_t buildCallRva = 0;
        std::uint32_t originalTargetRva = 0;
        std::string reason;
    };


    struct ClimaxCallResult {
        bool validated = false;
        std::uint32_t candidateCount = 0, validCount = 0;
        std::uint32_t functionRva = 0, functionEndRva = 0;
        std::uint32_t callRva = 0, originalTargetRva = 0;
        std::string reason;
    };
    [[nodiscard]] ClimaxCallResult ScanClimaxCall(
        std::span<const std::uint8_t> file, const ScanResult& ordinary);

    [[nodiscard]] SoundBuildCallResult ScanSoundBuildCall(std::span<const std::uint8_t> file);



    [[nodiscard]] ScanResult Scan(std::span<const std::uint8_t> file);
}
