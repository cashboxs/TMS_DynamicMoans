#include "PCH.h"
#include "core/RuntimeHelpers.h"
#include <Windows.h>
namespace TMS::DD {
std::filesystem::path GetGameDirectory() {
    wchar_t path[32768]{};
    if (GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path))) > 0)
        return std::filesystem::path(path).parent_path();
    return std::filesystem::current_path();
}
}
