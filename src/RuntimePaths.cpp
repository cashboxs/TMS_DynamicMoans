#include "PCH.h"
#include "RuntimePaths.h"
#include <Windows.h>
namespace TMS::DD {
std::filesystem::path GetPluginIniPathForLogging() {
    wchar_t path[32768]{};
    HMODULE m = GetModuleHandleW(L"TMS_DynamicMoans.dll");
    if (m && GetModuleFileNameW(m,path,static_cast<DWORD>(std::size(path))) > 0)
        return std::filesystem::path(path).parent_path() / "TMS_DynamicMoans.ini";
    return std::filesystem::path("Data/SKSE/Plugins/TMS_DynamicMoans.ini");
}
}
