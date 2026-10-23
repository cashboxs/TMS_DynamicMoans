#include "PCH.h"
#include "OStimMoanHook.h"
#include "OStimMoanAutoLocator.h"
#include "OStimMoanSuppressionPolicy.h"
#include "NativeMoanReplace.h"
#include "MoanHookOwner.h"
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#if defined(TMS_MOAN_STANDALONE)
#define MOAN_LOG(x) "DM_" x

#define MOAN_EVENT(...) do { \
    auto* dmLogger = spdlog::default_logger_raw(); \
    if(dmLogger && dmLogger->should_log(spdlog::level::debug)) \
        logger::debug(__VA_ARGS__); \
} while(false)
#else
#define MOAN_LOG(x) "DDF_" x
#define MOAN_EVENT(...) logger::info(__VA_ARGS__)
#endif
namespace logger = SKSE::log;
namespace TMS::DD::OStimMoanHook {
namespace {
    using PlaySoundFn = void (*)(void*, void*, void*, bool);
    using StartMoanCooldownFn = void (*)(void*);
    using AutoResult = OStimMoanAutoLocator::ScanResult;
    using SoundCallResult = OStimMoanAutoLocator::SoundBuildCallResult;



    PlaySoundFn sOriginalPlaySound = nullptr;
    StartMoanCooldownFn sOriginalCooldown = nullptr;
    std::uint32_t sActorOffset = 0;
    std::uint32_t sMoanOffset = 0;
    std::uint32_t sCooldownOffset = 0;
    std::uint32_t sTestActorFormID = 0;
    bool sSuppressTestMode = false;
    std::atomic_uint32_t sSuppressedCount{ 0 };
    void* sRelayMemory = nullptr;
    std::atomic_bool sHookInstalled{ false };
    void* sSoundRelayMemory = nullptr;
    void* sClimaxRelayMemory = nullptr;
    std::atomic_uint32_t sObservedCount{ 0 };

    [[nodiscard]] bool Contains(std::size_t end, std::size_t at, std::size_t len) noexcept
    {
        return at <= end && len <= end - at;
    }

    [[nodiscard]] bool ReadFileFully(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) return false;
        const auto length = stream.tellg();
        if (length < 512 || length > std::streamoff{128 * 1024 * 1024}) return false;
        bytes.resize(static_cast<std::size_t>(length));
        stream.seekg(0, std::ios::beg);
        return static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())));
    }

    struct PEHeaders {
        IMAGE_NT_HEADERS64 nt{};
        std::size_t sectionsAt = 0;
    };

    [[nodiscard]] bool DiskPEHeaders(std::span<const std::uint8_t> file, PEHeaders& out)
    {
        if (file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, file.data(), sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x2000)
            return false;
        const auto peAt = static_cast<std::size_t>(dos.e_lfanew);
        if (!Contains(file.size(), peAt, sizeof(out.nt))) return false;
        std::memcpy(&out.nt, file.data() + peAt, sizeof(out.nt));
        if (out.nt.Signature != IMAGE_NT_SIGNATURE ||
            out.nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            out.nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            out.nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            out.nt.FileHeader.NumberOfSections == 0 ||
            out.nt.FileHeader.NumberOfSections > 96 ||
            out.nt.OptionalHeader.SizeOfImage == 0 ||
            out.nt.OptionalHeader.SizeOfImage > 128 * 1024 * 1024)
            return false;
        out.sectionsAt = peAt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
            out.nt.FileHeader.SizeOfOptionalHeader;
        return Contains(file.size(), out.sectionsAt,
            static_cast<std::size_t>(out.nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER));
    }

    [[nodiscard]] bool MemoryRangeOK(HMODULE module, std::uintptr_t address,
        std::size_t len, bool requireExecutable) noexcept
    {
        if (!module || !len || address > (std::numeric_limits<std::uintptr_t>::max)() - len)
            return false;
        const auto end = address + len;
        while (address < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT || mbi.AllocationBase != module ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return false;
            const auto type = mbi.Protect & 0xFFu;
            if (requireExecutable && type != PAGE_EXECUTE && type != PAGE_EXECUTE_READ &&
                type != PAGE_EXECUTE_READWRITE && type != PAGE_EXECUTE_WRITECOPY)
                return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            if (mbi.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - begin)
                return false;
            const auto regionEnd = begin + mbi.RegionSize;
            if (regionEnd <= address) return false;
            address = (std::min)(end, regionEnd);
        }
        return true;
    }



    [[nodiscard]] bool MappedPEHeaders(HMODULE module, const PEHeaders& disk)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        if (!MemoryRangeOK(module, base, 0x1000, false)) return false;
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, reinterpret_cast<const void*>(base), sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x2000)
            return false;
        const auto peAt = static_cast<std::size_t>(dos.e_lfanew);
        if (!MemoryRangeOK(module, base + peAt, sizeof(IMAGE_NT_HEADERS64), false)) return false;
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, reinterpret_cast<const void*>(base + peAt), sizeof(nt));
        if (nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.NumberOfSections != disk.nt.FileHeader.NumberOfSections ||
            nt.FileHeader.SizeOfOptionalHeader != disk.nt.FileHeader.SizeOfOptionalHeader ||
            nt.OptionalHeader.SizeOfImage != disk.nt.OptionalHeader.SizeOfImage)
            return false;
        const auto loadedSectionsAt = peAt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
            nt.FileHeader.SizeOfOptionalHeader;
        const auto headerLen = static_cast<std::size_t>(nt.FileHeader.NumberOfSections) *
            sizeof(IMAGE_SECTION_HEADER);
        return MemoryRangeOK(module, base + loadedSectionsAt, headerLen, false);
    }





    [[nodiscard]] bool CompareFunction(HMODULE module, const std::vector<std::uint8_t>& file,
        const PEHeaders& pe, std::uint32_t start, std::uint32_t end)
    {
        if (start >= end || end > pe.nt.OptionalHeader.SizeOfImage || end - start > 0x4000)
            return false;
        const auto len = static_cast<std::size_t>(end - start);
        for (unsigned i = 0; i < pe.nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER s{};
            const auto at = pe.sectionsAt + i * sizeof(s);
            std::memcpy(&s, file.data() + at, sizeof(s));
            if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                start < s.VirtualAddress ||
                static_cast<std::uint64_t>(end) >
                    static_cast<std::uint64_t>(s.VirtualAddress) + s.SizeOfRawData)
                continue;
            const auto diskOffset = static_cast<std::uint64_t>(s.PointerToRawData) +
                (start - s.VirtualAddress);
            if (diskOffset > file.size() || len > file.size() - diskOffset)
                return false;
            const auto mapped = reinterpret_cast<std::uintptr_t>(module) + start;
            if (!MemoryRangeOK(module, mapped, len, true)) return false;
            return std::memcmp(reinterpret_cast<const void*>(mapped),
                file.data() + diskOffset, len) == 0;
        }
        return false;
    }

    [[nodiscard]] bool CheckMemory(HMODULE module, const std::vector<std::uint8_t>& file,
        const AutoResult& match, std::string& failedAt)
    {
        PEHeaders pe{};
        if (!DiskPEHeaders(file, pe)) { failedAt = "InvalidFilePE"; return false; }
        if (!MappedPEHeaders(module, pe)) { failedAt = "MappedPEMismatch"; return false; }
        struct Range { const char* name; std::uint32_t begin; std::uint32_t end; };
        const std::array<Range, 3> ranges{{
            {"loopSound", match.loopRva, match.loopEndRva},
            {"playSound", match.playRva, match.playEndRva},
            {"startMoanCooldown", match.cooldownRva, match.cooldownEndRva}
        }};
        for (const auto& r : ranges) {
            if (!CompareFunction(module, file, pe, r.begin, r.end)) {
                failedAt = r.name;
                return false;
            }
        }

        const auto patchSite = reinterpret_cast<std::uintptr_t>(module) + match.hookRva;
        if (match.hookRva < match.loopRva ||
            match.hookRva + 5 > match.loopEndRva ||
            !MemoryRangeOK(module, patchSite, 5, true) ||
            *reinterpret_cast<const std::uint8_t*>(patchSite) != 0xE9) {
            failedAt = "OrdinaryTailBranch";
            return false;
        }
        std::int32_t disp{};
        std::memcpy(&disp, reinterpret_cast<const void*>(patchSite + 1), sizeof(disp));
        const auto target = static_cast<std::int64_t>(match.hookRva) + 5 + disp;
        if (target != match.playRva) {
            failedAt = "OrdinaryTailTarget";
            return false;
        }
        return true;
    }

        bool RelativeJumpDisplacement(std::uintptr_t source, std::uintptr_t destination,
            std::int32_t& displacement) noexcept
        {
            const auto delta = static_cast<std::int64_t>(destination) -
                static_cast<std::int64_t>(source + 5);
            if (delta < (std::numeric_limits<std::int32_t>::min)() ||
                delta > (std::numeric_limits<std::int32_t>::max)()) {
                return false;
            }
            displacement = static_cast<std::int32_t>(delta);
            return true;
        }




        void* AllocateNearbyRelay(std::uintptr_t patchSite)
        {
            SYSTEM_INFO systemInfo{};
            GetSystemInfo(&systemInfo);
            const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
            const auto pageSize = static_cast<std::size_t>(systemInfo.dwPageSize);
            if (!granularity || !pageSize) {
                return nullptr;
            }
            const auto origin = (patchSite / granularity) * granularity;

            constexpr std::uintptr_t maxDistance = 0x7FF00000;
            const auto minimum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
            const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
            for (std::uintptr_t distance = granularity; distance <= maxDistance; distance += granularity) {
                for (unsigned direction = 0; direction < 2; ++direction) {
                    std::uintptr_t candidate = 0;
                    if (direction == 0) {
                        if (distance > (std::numeric_limits<std::uintptr_t>::max)() - origin) {
                            continue;
                        }
                        candidate = origin + distance;
                    } else {
                        if (origin < distance) {
                            continue;
                        }
                        candidate = origin - distance;
                    }
                    if (candidate < minimum || candidate > maximum || maximum - candidate < pageSize) {
                        continue;
                    }
                    std::int32_t displacement = 0;
                    if (!RelativeJumpDisplacement(patchSite, candidate, displacement)) {
                        continue;
                    }
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (!VirtualQuery(reinterpret_cast<void*>(candidate), &mbi, sizeof(mbi)) ||
                        mbi.State != MEM_FREE) {
                        continue;
                    }
                    const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                    if (candidate < regionBase || candidate - regionBase > mbi.RegionSize ||
                        mbi.RegionSize - (candidate - regionBase) < pageSize) {
                        continue;
                    }
                    if (auto* allocated = VirtualAlloc(reinterpret_cast<void*>(candidate), pageSize,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                        return allocated;
                    }
                }
            }
            return nullptr;
        }



        bool PrepareAbsoluteRelay(void* relay, std::uintptr_t observer)
        {
            std::array<std::uint8_t, 14> code = {
                0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
                0, 0, 0, 0, 0, 0, 0, 0
            };
            static_assert(sizeof(std::uintptr_t) == 8);
            std::memcpy(code.data() + 6, &observer, sizeof(observer));
            std::memcpy(relay, code.data(), code.size());
            DWORD oldProtection = 0;
            if (!VirtualProtect(relay, code.size(), PAGE_EXECUTE_READ, &oldProtection)) {
                return false;
            }
            return FlushInstructionCache(GetCurrentProcess(), relay, code.size()) != FALSE;
        }




        bool PatchFiveByteInstruction(std::uintptr_t site,std::uintptr_t relay,
            const std::array<std::uint8_t,5>& expected,std::uint8_t opcode,DWORD& errorCode)
        {
            std::int32_t disp{};
            if(!RelativeJumpDisplacement(site,relay,disp)) {
                errorCode=ERROR_ARITHMETIC_OVERFLOW;return false;
            }
            std::array<std::uint8_t,5> replacement{opcode,0,0,0,0};
            std::memcpy(replacement.data()+1,&disp,4);
            DWORD old{};
            if(!VirtualProtect(reinterpret_cast<void*>(site),5,PAGE_EXECUTE_READWRITE,&old)){
                errorCode=GetLastError();return false;
            }
            const bool unchanged=std::memcmp(reinterpret_cast<const void*>(site),
                expected.data(),5)==0 && expected[0]==opcode;
            if(unchanged)std::memcpy(reinterpret_cast<void*>(site),replacement.data(),5);
            DWORD discard{};
            const bool restored=VirtualProtect(reinterpret_cast<void*>(site),5,old,&discard)!=FALSE;
            if(!unchanged){errorCode=ERROR_REVISION_MISMATCH;return false;}
            if(!restored || !FlushInstructionCache(GetCurrentProcess(),
                reinterpret_cast<void*>(site),5)) {
                logger::warn(MOAN_LOG("NATIVE_MOAN_HOOK: state=installed-warning ")
                    "reason=RestoreProtectionOrFlushFailed");
            }
            errorCode=ERROR_SUCCESS;return true;
        }

        bool PatchOrdinaryTailJump(std::uintptr_t patchSite, std::uintptr_t relay,
            const std::array<std::uint8_t, 5>& expected, DWORD& errorCode)
        {
            std::int32_t displacement = 0;
            if (!RelativeJumpDisplacement(patchSite, relay, displacement)) {
                errorCode = ERROR_ARITHMETIC_OVERFLOW;
                return false;
            }
            std::array<std::uint8_t, 5> replacement{ 0xE9, 0, 0, 0, 0 };
            std::memcpy(replacement.data() + 1, &displacement, sizeof(displacement));
            DWORD oldProtection = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(patchSite), replacement.size(),
                PAGE_EXECUTE_READWRITE, &oldProtection)) {
                errorCode = GetLastError();
                return false;
            }
            const bool stillOriginal = std::memcmp(reinterpret_cast<const void*>(patchSite), expected.data(), expected.size()) == 0;
            if (stillOriginal) {
                std::memcpy(reinterpret_cast<void*>(patchSite), replacement.data(), replacement.size());
            }
            DWORD discarded = 0;
            const bool restoredProtection = VirtualProtect(reinterpret_cast<void*>(patchSite),
                replacement.size(), oldProtection, &discarded) != FALSE;
            if (!stillOriginal) {
                errorCode = ERROR_REVISION_MISMATCH;
                return false;
            }


            if (!restoredProtection) {
                logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=installed-warning reason=RestorePageProtectionFailed win32={}"),
                    GetLastError());
            }
            if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patchSite),
                replacement.size())) {
                logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=installed-warning reason=FlushInstructionCacheFailed win32={}"),
                    GetLastError());
            }
            errorCode = ERROR_SUCCESS;
            return true;
        }




    void ObserveClimaxSound(void* self, void* reactionSet, void* partner, bool ignoreChecks)
    {
        if (!sOriginalPlaySound) return;
        auto* actor=self ? *reinterpret_cast<RE::Actor**>(
            static_cast<std::byte*>(self)+sActorOffset) : nullptr;
        NativeMoanReplace::ClimaxScope scope(actor ? actor->GetFormID() : 0u);
        if(!scope.Suppressed()) {
            sOriginalPlaySound(self,reactionSet,partner,ignoreChecks);
        }
    }




    void ObserveOrdinaryMoan(void* self, void* reactionSet, void* partner, bool ignoreChecks)
    {
        const bool installed = sHookInstalled.load(std::memory_order_acquire);
        const auto original = sOriginalPlaySound;
        if (!original) return;
        const bool ordinaryReaction = self &&
            reactionSet == static_cast<void*>(static_cast<std::byte*>(self) + sMoanOffset);
        if (!installed || !ordinaryReaction || ignoreChecks) {
            original(self, reactionSet, partner, ignoreChecks);
            return;
        }
        auto* actor = *reinterpret_cast<RE::Actor**>(static_cast<std::byte*>(self) + sActorOffset);
        const auto formID = actor ? actor->GetFormID() : 0u;




        const auto seq = sObservedCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool logThis = seq <= 256 || seq % 100 == 0;
        const auto* timerAddress = static_cast<std::byte*>(self) + sCooldownOffset;
        std::int32_t timerBefore{};
        std::memcpy(&timerBefore, timerAddress, sizeof(timerBefore));
        const auto cooldown = sOriginalCooldown;


        if (timerBefore <= 0 && sOriginalCooldown &&
            NativeMoanReplace::ShouldSuppressForVoiceGuard(formID)) {
            sOriginalCooldown(self);
            std::int32_t timerAfter{};
            std::memcpy(&timerAfter, timerAddress, sizeof(timerAfter));
            if (OStimMoanSuppressionPolicy::CooldownRearmed(timerAfter)) {
                logger::info(MOAN_LOG("MOAN_COORD: actor={:08X} decision=SUPPRESS ")
                    "reason=DDFVoiceGuard nativeCooldownMs={}",formID,timerAfter);
                return;
            }
            logger::warn(MOAN_LOG("MOAN_COORD: actor={:08X} decision=PASS ")
                "reason=NativeCooldownNotRearmed cooldownAfter={}",formID,timerAfter);
        }
#if defined(TMS_MOAN_STANDALONE)




        auto* partnerActor = reinterpret_cast<RE::Actor*>(partner);
        NativeMoanReplace::OrdinaryScope nativeScope(formID, actor, partnerActor);
#else
        NativeMoanReplace::OrdinaryScope nativeScope(formID, actor);
#endif
        const bool shouldTry = OStimMoanSuppressionPolicy::ShouldAttempt(
            sSuppressTestMode, sTestActorFormID, formID, ordinaryReaction,
            ignoreChecks, cooldown != nullptr, timerBefore);
        if (shouldTry) {


            cooldown(self);
            std::int32_t timerAfter{};
            std::memcpy(&timerAfter, timerAddress, sizeof(timerAfter));
            if (OStimMoanSuppressionPolicy::CooldownRearmed(timerAfter)) {
                const auto count = sSuppressedCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (logThis) {
                    try {
                        MOAN_EVENT(MOAN_LOG("OSTIM_MOAN_HOOK: seq={} kind=ordinary_moan ")
                            "actor={:08X} decision=SUPPRESS mode=suppress-test "
                            "cooldownBefore={} cooldownAfter={} cooldownRearmed=true "
                            "suppressedTotal={} locator=structural",
                            seq, formID, timerBefore, timerAfter, count);
                    } catch (...) {

                    }
                }
                return;
            }


            try {
                logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: seq={} actor={:08X} ")
                    "decision=PASS reason=CooldownNotRearmed before={} after={}",
                    seq, formID, timerBefore, timerAfter);
            } catch (...) {}
        } else if (logThis) {
            try {
                MOAN_EVENT(MOAN_LOG("OSTIM_MOAN_HOOK: seq={} kind=ordinary_moan ")
                    "actor={:08X} decision=PASS mode={} cooldownBefore={} locator=structural",
                    seq, formID, sSuppressTestMode ? "suppress-test" : "observe-only",
                    timerBefore);
            } catch (...) {}
        }
        original(self, reactionSet, partner, ignoreChecks);


        nativeScope.AdjustNativeCooldown(const_cast<std::byte*>(timerAddress));
    }

}

void TryInstallAutoHook(bool enabled, bool suppressTestMode, std::uint32_t testActorFormID)
{
    if (!enabled) {
        logger::info(MOAN_LOG("OSTIM_MOAN_HOOK: state=disabled setting=HybridMoanProbe.Enabled action=NO_PATCH"));
        return;
    }
    if (sHookInstalled.load(std::memory_order_acquire)) return;
    if (!MoanHookOwner::Acquire()) return;
    try {
        const HMODULE module = GetModuleHandleW(L"OStim.dll");
        if (!module) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=OStimNotLoaded action=NO_PATCH"));
            return;
        }
        std::wstring path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!n || n >= path.size()) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=ModulePathUnavailable action=NO_PATCH"));
            return;
        }
        path.resize(n);
        std::vector<std::uint8_t> file;
        if (!ReadFileFully(std::filesystem::path(path), file)) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=CannotReadBackingDLL action=NO_PATCH"));
            return;
        }
        const auto result = OStimMoanAutoLocator::Scan(file);
        if (!result.validated) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=ScanRejected detail='{}' ")
                "candidates={} valid={} action=NO_PATCH",
                result.reason, result.candidateCount, result.validatedCount);
            return;
        }
        std::string failedAt;
        if (!CheckMemory(module, file, result, failedAt)) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=MappedCodeMismatch ")
                "region={} hookRVA=0x{:X} action=NO_PATCH",
                failedAt, result.hookRva);
            return;
        }
        SoundCallResult soundCall{};
        bool soundCallSafe=false;
        if (NativeMoanReplace::IsEnabled()) {
            soundCall=OStimMoanAutoLocator::ScanSoundBuildCall(file);
            PEHeaders pe{};
            soundCallSafe=soundCall.validated && DiskPEHeaders(file,pe) &&
                CompareFunction(module,file,pe,soundCall.gameSoundPlayRva,
                    soundCall.gameSoundPlayEndRva);
            const auto soundAt=reinterpret_cast<std::uintptr_t>(module)+soundCall.buildCallRva;
            if (soundCallSafe) {
                std::int32_t delta{};
                if (!MemoryRangeOK(module,soundAt,5,true) ||
                    *reinterpret_cast<const std::uint8_t*>(soundAt)!=0xE8) soundCallSafe=false;
                else {
                    std::memcpy(&delta,reinterpret_cast<const void*>(soundAt+1),4);
                    const auto target=static_cast<std::int64_t>(soundCall.buildCallRva)+5+delta;
                    if(target!=soundCall.originalTargetRva) soundCallSafe=false;
                }
            }
            logger::debug(MOAN_LOG("NATIVE_MOAN_HOOK: stage=preflight state={} ")
                "candidates={} valid={} gameSoundRVA=0x{:X} buildCallRVA=0x{:X} "
                "originalTargetRVA=0x{:X} reason='{}'",
                soundCallSafe?"validated":"rejected",soundCall.candidateCount,
                soundCall.validCount,soundCall.gameSoundPlayRva,
                soundCall.buildCallRva,soundCall.originalTargetRva,soundCall.reason);
        }

        const auto climaxCall=OStimMoanAutoLocator::ScanClimaxCall(file,result);
        PEHeaders climaxPE{};
        bool climaxCallSafe=climaxCall.validated && DiskPEHeaders(file,climaxPE) &&
            CompareFunction(module,file,climaxPE,climaxCall.functionRva,climaxCall.functionEndRva);
        if(climaxCallSafe) {
            const auto at=reinterpret_cast<std::uintptr_t>(module)+climaxCall.callRva;
            std::int32_t disp{};
            if(!MemoryRangeOK(module,at,5,true) ||
               *reinterpret_cast<const std::uint8_t*>(at)!=0xE8) climaxCallSafe=false;
            else {
                std::memcpy(&disp,reinterpret_cast<const void*>(at+1),4);
                if(static_cast<std::int64_t>(climaxCall.callRva)+5+disp !=
                   climaxCall.originalTargetRva) climaxCallSafe=false;
            }
        }
        logger::debug(MOAN_LOG("MOAN_CLIMAX: stage=preflight state={} candidates={} valid={} ")
            "callRVA=0x{:X} functionRVA=0x{:X} reason='{}'",
            climaxCallSafe?"validated":"rejected",climaxCall.candidateCount,
            climaxCall.validCount,climaxCall.callRva,climaxCall.functionRva,climaxCall.reason);
        logger::debug(MOAN_LOG("OSTIM_MOAN_HOOK: state=validated mode=memory-checked ")
            "candidates={} valid={} loopRVA=0x{:X} hookRVA=0x{:X} "
            "playRVA=0x{:X} cooldownRVA=0x{:X} actorOffset=0x{:X} "
            "partnerOffset=0x{:X} moanOffset=0x{:X} cooldownOffset=0x{:X} "
            "source=disk-and-mapped-memory",
            result.candidateCount, result.validatedCount, result.loopRva, result.hookRva,
            result.playRva, result.cooldownRva, result.actorOffset, result.partnerOffset,
            result.moanOffset, result.cooldownOffset);

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto patchSite = base + result.hookRva;
        const auto playbackAddress = base + result.playRva;
        const auto cooldownAddress = base + result.cooldownRva;
        std::array<std::uint8_t, 5> original{};
        std::memcpy(original.data(), reinterpret_cast<const void*>(patchSite), original.size());
        void* relay = AllocateNearbyRelay(patchSite);
        if (!relay) {
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=NoReachableRelay ")
                "hookRVA=0x{:X} action=NO_PATCH", result.hookRva);
            return;
        }
        const auto relayAddress = reinterpret_cast<std::uintptr_t>(relay);
        std::int32_t rel32 = 0;
        if (!RelativeJumpDisplacement(patchSite, relayAddress, rel32) ||
            !PrepareAbsoluteRelay(relay, reinterpret_cast<std::uintptr_t>(&ObserveOrdinaryMoan))) {
            const auto code = GetLastError();
            VirtualFree(relay, 0, MEM_RELEASE);
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=RelayPreparationFailed ")
                "win32={} action=NO_PATCH", code);
            return;
        }


        sOriginalPlaySound = reinterpret_cast<PlaySoundFn>(playbackAddress);
        sOriginalCooldown = reinterpret_cast<StartMoanCooldownFn>(cooldownAddress);
        sActorOffset = result.actorOffset;
        sMoanOffset = result.moanOffset;
        sCooldownOffset = result.cooldownOffset;
        sSuppressTestMode = suppressTestMode && testActorFormID != 0 && testActorFormID != 0x14u;
        sTestActorFormID = sSuppressTestMode ? testActorFormID : 0;
        DWORD errorCode = ERROR_SUCCESS;
        if (!PatchOrdinaryTailJump(patchSite, relayAddress, original, errorCode)) {
            VirtualFree(relay, 0, MEM_RELEASE);
            logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=PatchFailed ")
                "win32={} action=NO_PATCH", errorCode);
            return;
        }
        sRelayMemory = relay;
        sHookInstalled.store(true, std::memory_order_release);



        if (soundCallSafe) {
            const auto soundAt=base+soundCall.buildCallRva;
            const auto nativeTarget=base+soundCall.originalTargetRva;
            std::array<std::uint8_t,5> soundOriginal{};
            std::memcpy(soundOriginal.data(),reinterpret_cast<const void*>(soundAt),5);
            if (auto* soundRelay=AllocateNearbyRelay(soundAt)) {
                const auto newRelay=reinterpret_cast<std::uintptr_t>(soundRelay);
                const bool ready=PrepareAbsoluteRelay(soundRelay,
                    reinterpret_cast<std::uintptr_t>(&NativeMoanReplace::OnNativeBuildSound));
                std::int32_t displacement{};
                const bool reachable=RelativeJumpDisplacement(soundAt,newRelay,displacement);
                if (ready && reachable) {
                    NativeMoanReplace::SetOriginalBuildFn(
                        reinterpret_cast<NativeMoanReplace::NativeBuildFn>(nativeTarget));
                    DWORD ec=0;
                    if (PatchFiveByteInstruction(soundAt,newRelay,soundOriginal,0xE8,ec)) {
                        sSoundRelayMemory=soundRelay;
                        NativeMoanReplace::SetNativeCallHookInstalled(true);
                        logger::debug(MOAN_LOG("NATIVE_MOAN_HOOK: state=installed mode=native-replace ")
                            "gameSoundRVA=0x{:X} callRVA=0x{:X} "
                            "originalTargetRVA=0x{:X} actorScoped=true",
                            soundCall.gameSoundPlayRva,soundCall.buildCallRva,
                            soundCall.originalTargetRva);
                    }else{
                        VirtualFree(soundRelay,0,MEM_RELEASE);
                        logger::warn(MOAN_LOG("NATIVE_MOAN_HOOK: state=inactive ")
                            "reason=CallPatchFailed win32={} action=PASS",ec);
                    }
                }else{
                    if (!ready) {


                    }
                    VirtualFree(soundRelay,0,MEM_RELEASE);
                    logger::warn(MOAN_LOG("NATIVE_MOAN_HOOK: state=inactive ")
                        "reason=RelayPreparationFailed action=PASS");
                }
            }else{
                logger::warn(MOAN_LOG("NATIVE_MOAN_HOOK: state=inactive ")
                    "reason=NoReachableRelay action=PASS");
            }
        } else if (NativeMoanReplace::IsEnabled()) {
            logger::warn(MOAN_LOG("NATIVE_MOAN_HOOK: state=inactive ")
                "reason=SoundBuildValidationFailed action=PASS");
        }



        if (climaxCallSafe && NativeMoanReplace::HasClimaxFiles()) {
            const auto callAt=base+climaxCall.callRva;
            std::array<std::uint8_t,5> expected{};
            std::memcpy(expected.data(),reinterpret_cast<const void*>(callAt),5);
            if(auto* relay=AllocateNearbyRelay(callAt)) {
                const auto dest=reinterpret_cast<std::uintptr_t>(relay);
                std::int32_t displacement{};
                if (RelativeJumpDisplacement(callAt,dest,displacement) &&
                    PrepareAbsoluteRelay(relay,reinterpret_cast<std::uintptr_t>(&ObserveClimaxSound))) {
                    DWORD ec=0;
                    if(PatchFiveByteInstruction(callAt,dest,expected,0xE8,ec)) {
                        sClimaxRelayMemory=relay;
                        NativeMoanReplace::SetClimaxHookInstalled(true);
                        logger::debug(MOAN_LOG("MOAN_CLIMAX: state=installed callRVA=0x{:X} ")
                            "mode=actor-scoped-native-replace",climaxCall.callRva);
                    } else {
                        VirtualFree(relay,0,MEM_RELEASE);
                        logger::warn(MOAN_LOG("MOAN_CLIMAX: state=inactive reason=PatchFailed win32={}"),ec);
                    }
                } else {
                    VirtualFree(relay,0,MEM_RELEASE);
                    logger::warn(MOAN_LOG("MOAN_CLIMAX: state=inactive reason=RelayFailed"));
                }
            } else logger::warn(MOAN_LOG("MOAN_CLIMAX: state=inactive reason=NoReachableRelay"));
        } else if(NativeMoanReplace::IsEnabled()) {
            logger::debug(MOAN_LOG("MOAN_CLIMAX: state=disabled reason={}"),
                !NativeMoanReplace::HasClimaxFiles()?"NoClimaxWavsOrDisabled":"StructureNotValidated");
        }
        logger::debug(MOAN_LOG("OSTIM_MOAN_HOOK: state=installed mode={} ")
            "hookRVA=0x{:X} originalRVA=0x{:X} cooldownRVA=0x{:X} relay={:016X} "
            "displacement={} testActor={:08X} "
            "locator=structural fix=near-relay",
            sSuppressTestMode ? "suppress-test" : "observe-only",
            result.hookRva, result.playRva, result.cooldownRva,
            relayAddress, rel32, sTestActorFormID);
#if defined(TMS_MOAN_STANDALONE)
        logger::info("DM_HOOK: ordinary={} GameSound={} climax={}",
            sHookInstalled.load() ? "ready" : "inactive",
            NativeMoanReplace::IsReady() ? "ready" : "inactive",
            NativeMoanReplace::IsClimaxHookAvailable() ? "ready" : "inactive");
#endif
    } catch (const std::exception& e) {
        logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=SetupException ")
            "what='{}' action=NO_PATCH", e.what());
    } catch (...) {
        logger::warn(MOAN_LOG("OSTIM_MOAN_HOOK: state=inactive reason=UnknownSetupException action=NO_PATCH"));
    }
}
}
