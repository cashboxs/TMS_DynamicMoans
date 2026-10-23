#include "PCH.h"
#include "MoanMenu.h"
#include "SKSEMenuFramework.h"
#include "NativeMoanReplace.h"
#include "RuntimePaths.h"
#include <Windows.h>
#include <array>
#include <cwchar>
#include <spdlog/spdlog.h>

namespace TMS::DD::MoanMenu {
namespace {
    bool sRegistered = false;
    bool sEnabledIni = true;
    constexpr std::array<const wchar_t*,7> kLogWide{
        L"off",L"critical",L"error",L"warn",L"info",L"debug",L"trace"};
    constexpr const char* kLogNames[]={"Off","Critical","Error","Warning","Info","Debug","Trace"};
    constexpr std::array<spdlog::level::level_enum,7> kLevels{
        spdlog::level::off,spdlog::level::critical,spdlog::level::err,
        spdlog::level::warn,spdlog::level::info,spdlog::level::debug,spdlog::level::trace};
    int sLogIndex=4;
    std::wstring Ini() {return GetPluginIniPathForLogging().wstring();}
    bool Save(const wchar_t* section,const wchar_t* key,const wchar_t* value) {
        const auto ini=Ini();
        if(WritePrivateProfileStringW(section,key,value,ini.c_str())) return true;
        spdlog::warn("DM_MENU: INI write failed win32={}",GetLastError());
        return false;
    }
    void __stdcall SettingsPage() {
        ImGuiMCP::TextWrapped("Player-only OStim native WAV replacement. Male/Female library is selected automatically from player sex.");
        bool enabled=sEnabledIni;
        if(ImGuiMCP::Checkbox("Enable Dynamic Moans##dm_enabled",&enabled)) {
            if(Save(L"NativeMoanReplace",L"Enabled",enabled?L"1":L"0"))
                sEnabledIni=enabled;
        }
        ImGuiMCP::TextDisabled("Enable/disable takes effect after restarting Skyrim.");
        ImGuiMCP::SeparatorText("Logging");
        int selected=sLogIndex;
        if(ImGuiMCP::Combo("Log Level##dm_log",&selected,kLogNames,7)) {
            if(Save(L"Logging",L"Level",kLogWide[static_cast<std::size_t>(selected)])) {
                sLogIndex=selected;
                spdlog::set_level(kLevels[static_cast<std::size_t>(selected)]);
                spdlog::info("DM_MENU: logLevel={}",kLogNames[selected]);
            }
        }
        ImGuiMCP::TextDisabled("Debug and Trace provide per-event diagnostics.");
        ImGuiMCP::TextDisabled("Sound status: %s",NativeMoanReplace::IsReady()?"Ready":"Inactive");
    }
}
void LoadSettings() {
    const auto ini=Ini();
    sEnabledIni=GetPrivateProfileIntW(L"NativeMoanReplace",L"Enabled",1,ini.c_str())!=0;
    wchar_t level[32]{};
    GetPrivateProfileStringW(L"Logging",L"Level",L"info",level,
        static_cast<DWORD>(std::size(level)),ini.c_str());
    sLogIndex=4;
    for(std::size_t i=0;i<kLogWide.size();++i)
        if(_wcsicmp(level,kLogWide[i])==0){sLogIndex=static_cast<int>(i);break;}
}
void Register() {
    if(sRegistered) return;
    if(!GetMenuFrameworkModule()) {
        spdlog::info("DM_MENU: state=unavailable reason=SKSEMenuFrameworkMissing fallback=INI");
        return;
    }
    SKSEMenuFramework::SetSection("Dynamic Moans");
    SKSEMenuFramework::AddSectionItem("Settings",SettingsPage);
    sRegistered=true;
    spdlog::debug("DM_MENU: state=registered sections=1 controls=2");
}
}
