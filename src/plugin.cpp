#include "PCH.h"
#include "Logger.h"
#include "NativeMoanReplace.h"
#include "OStimMoanHook.h"
#include "PPAEnhancement.h"
#include "MoanMenu.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
namespace {
void OnSKSEMessage(SKSE::MessagingInterface::Message* msg) {
    if(!msg) return;
    if(msg->type==SKSE::MessagingInterface::kDataLoaded) {
        TMS::DD::PrintStartupBanner();



        if (GetModuleHandleW(L"TMS_DynamicDialogue.dll")) {
            SKSE::log::warn("TMS_MOAN_OWNER: state=STANDBY winner=TMS_DynamicDialogue "
                "decision=SKIP_DM_HOOK reason=DDFPresent");
            return;
        }
        TMS::DD::NativeMoanReplace::LoadSettings();
        TMS::DD::MoanMenu::LoadSettings();
        TMS::DD::MoanMenu::Register();
        if(TMS::DD::NativeMoanReplace::IsEnabled()) {
            TMS::DD::PPAEnhancement::Connect();

            TMS::DD::OStimMoanHook::TryInstallAutoHook(true,false,0);
        }
    } else if(msg->type==SKSE::MessagingInterface::kPreLoadGame ||
              msg->type==SKSE::MessagingInterface::kNewGame) {
        TMS::DD::PPAEnhancement::ResetSnapshots();
    }
}
}
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    TMS::DD::InitLogger();
    auto* messaging=SKSE::GetMessagingInterface();
    if(!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
        SKSE::log::critical("DM_INIT: messaging unavailable");
        return false;
    }
    SKSE::log::debug("DM_INIT: loaded; public=1.0 internal=1.0.1; no ESP/Papyrus");
    if (const auto logger = spdlog::default_logger()) logger->flush();
    return true;
}
