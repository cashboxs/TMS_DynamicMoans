#include "PCH.h"
#include "MoanHookOwner.h"
#include <Windows.h>
#include <atomic>
#include <cwchar>
namespace logger = SKSE::log;
namespace TMS::DD::MoanHookOwner {
namespace {
    HANDLE sMapping = nullptr;
    volatile LONG* sOwner = nullptr;
#if defined(TMS_MOAN_STANDALONE)
    constexpr LONG kMyOwner = 2;
    constexpr const char* kMyName = "TMS_DynamicMoans";
#else
    constexpr LONG kMyOwner = 1;
    constexpr const char* kMyName = "TMS_DynamicDialogue";
#endif
}
bool Acquire() noexcept {
#if defined(TMS_MOAN_STANDALONE)


    if (GetModuleHandleW(L"TMS_DynamicDialogue.dll")) {
        logger::warn("TMS_MOAN_OWNER: state=STANDBY winner=TMS_DynamicDialogue "
            "decision=SKIP_DM_HOOK reason=DDFPresent");
        return false;
    }
#endif
    if (!sOwner) {
        wchar_t name[100]{};
        std::swprintf(name, std::size(name), L"Local\\TMS_OStimNativeMoanOwner_v1_%08lX",
            static_cast<unsigned long>(GetCurrentProcessId()));
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, 4096, name);
        if (!mapping) {
            logger::critical("TMS_MOAN_OWNER: state=DENY reason=CreateFileMappingFailed win32={}",GetLastError());
            return false;
        }
        void* mapped = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
        if (!mapped) {
            logger::critical("TMS_MOAN_OWNER: state=DENY reason=MapViewFailed win32={}",GetLastError());
            CloseHandle(mapping);
            return false;
        }
        sMapping = mapping;
        sOwner = static_cast<volatile LONG*>(mapped);
    }
    const LONG previous = InterlockedCompareExchange(sOwner, kMyOwner, 0);
    if (previous != 0 && previous != kMyOwner) {
        logger::critical("TMS_MOAN_OWNER: state=CONFLICT requested={} owner={} decision=SKIP_DM_HOOK",
            kMyName,previous);
        return false;
    }
    logger::debug("TMS_MOAN_OWNER: state=RESERVED owner={} decision=ALLOW", kMyName);


    return true;
}
}
