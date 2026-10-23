#include "PCH.h"
#include "Logger.h"
#include "RuntimePaths.h"
#include "third_party/AccuratePenetrationAPI.h"
#include <Windows.h>
#include <winver.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace TMS::DD {
namespace {


    std::string ModuleVersion(const wchar_t* dllName) {
        const HMODULE module = GetModuleHandleW(dllName);
        if (!module) return "None";




        if (const auto* raw = GetProcAddress(module, "SKSEPlugin_Version")) {
            std::uint32_t fields[2]{};
            std::memcpy(fields, reinterpret_cast<const void*>(raw), sizeof(fields));
            if (fields[0] == 1 && fields[1] != 0) {
                const auto version = REL::Version::unpack(fields[1]);
                return version.string(".");
            }
        }


        std::vector<wchar_t> widePath(32768);
        const DWORD length = GetModuleFileNameW(module,widePath.data(),
            static_cast<DWORD>(widePath.size()));
        if (!length || length >= widePath.size()) return "Unknown (DLL loaded)";
        DWORD ignored = 0;
        const DWORD bytes = GetFileVersionInfoSizeW(widePath.data(), &ignored);
        if (!bytes || bytes > 4 * 1024 * 1024) return "Unknown (DLL loaded)";
        std::vector<std::uint8_t> data(bytes);
        if (!GetFileVersionInfoW(widePath.data(), 0, bytes, data.data()))
            return "Unknown (DLL loaded)";
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoSize = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
            !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
            return "Unknown (DLL loaded)";
        return fmt::format("{}.{}.{}.{}",
            HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    }

    spdlog::level::level_enum ReadLogLevel(const std::filesystem::path& iniPath) {
        wchar_t text[32]{};
        const auto ini = iniPath.wstring();
        GetPrivateProfileStringW(L"Logging", L"Level", L"info", text,
            static_cast<DWORD>(std::size(text)),ini.c_str());
        if (_wcsicmp(text,L"trace") == 0) return spdlog::level::trace;
        if (_wcsicmp(text,L"debug") == 0) return spdlog::level::debug;
        if (_wcsicmp(text,L"warn") == 0 || _wcsicmp(text,L"warning") == 0)
            return spdlog::level::warn;
        if (_wcsicmp(text,L"error") == 0) return spdlog::level::err;
        if (_wcsicmp(text,L"critical") == 0) return spdlog::level::critical;
        if (_wcsicmp(text,L"off") == 0) return spdlog::level::off;
        return spdlog::level::info;
    }
}

void InitLogger() {
    const auto folder = SKSE::log::log_directory();
    if (!folder) {
        OutputDebugStringA("TMS Dynamic Moans: SKSE log directory unavailable\n");
        return;
    }
    try {
        const auto iniPath = GetPluginIniPathForLogging();
        auto logger = spdlog::basic_logger_mt("TMS_DynamicMoans",
            (*folder / "TMS_DynamicMoans.log").string(),true);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_level(ReadLogLevel(iniPath));


        spdlog::flush_on(spdlog::level::info);


        spdlog::debug("DM_BOOT: public=1.0 internal=1.0.1 stage=SKSEPluginLoad ini='{}'",
            iniPath.string());
        spdlog::default_logger()->flush();
    } catch (const std::exception& e) {
        OutputDebugStringA("TMS Dynamic Moans: logger setup failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
    }
}

void PrintStartupBanner() {


    const auto skyrim = REL::Module::get().version().string(".");
    const auto ostim  = ModuleVersion(L"OStim.dll");
    const auto ppa    = ModuleVersion(AccuratePenetration::API::kPluginDLL);
    const auto levelView = spdlog::level::to_string_view(spdlog::get_level());
    const std::string levelName(levelView.data(), levelView.size());
    spdlog::info(
        "\n"
        "==============================================================\n"
        "           Dynamic Moans for OStim Standalone\n"
        "--------------------------------------------------------------\n"
        "  Plugin          : 1.0 (internal 1.0.1)\n"
        "  Skyrim runtime  : {}\n"
        "  OStim           : {}\n"
        "  PPA             : {}\n"
        "  Log level       : {}\n"
        "==============================================================",
        skyrim, ostim, ppa, levelName);
    if (const auto logger = spdlog::default_logger()) logger->flush();
}
}
