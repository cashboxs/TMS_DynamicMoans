#include "PCH.h"
#include "PPAEnhancement.h"
#include "PPAContactAggregation.h"
#include "third_party/AccuratePenetrationAPI.h"
#include "RuntimePaths.h"
#include "SKSEMenuFramework.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>
#if defined(TMS_MOAN_STANDALONE)
#define MOAN_LOG(x) "DM_" x
#else
#define MOAN_LOG(x) "DDF_" x
#endif
namespace logger=SKSE::log;
namespace TMS::DD::PPAEnhancement {
namespace {
namespace AP=AccuratePenetration::API;
using Clock=std::chrono::steady_clock;
struct Interaction {
    std::uint32_t partner=0;
    MoanPhysicalPolicy::Site site=MoanPhysicalPolicy::Site::None;
    float depth=0;
};
struct Receiver {
    std::uint32_t id=0,context=0;
    Clock::time_point updated{};
    std::vector<Interaction> others;
    bool hasSelf=false;
    Interaction self;
};
std::mutex sMutex;
std::unordered_map<std::uint32_t,Receiver> sReceivers;
const AP::InterfaceV1* sAPI=nullptr;
AP::ListenerHandle sListener=0;
std::atomic_bool sEnabled{false};
std::atomic_bool sApply{false};
std::atomic<int> sMaxAgeMs{2500};
std::atomic_bool sConnected{false};
std::atomic_uint32_t sEvents{0};

std::uint32_t FormID(const RE::ActorHandle& h) noexcept {
    const auto resolved=h.get();
    return resolved ? resolved.get()->GetFormID() : 0u;
}
float Finite(float value) noexcept {return std::isfinite(value) ? value : 0.0f;}
Interaction CopyPartner(const AP::InteractionPartner& src) noexcept {
    return {FormID(src.actor),static_cast<MoanPhysicalPolicy::Site>(src.site),
        Finite(src.penetrationDepth)};
}
void __cdecl OnEvent(const AP::AnimationUpdateEvent* event,void*) noexcept {

    if(!sEnabled.load(std::memory_order_relaxed)) return;
    if(!event || event->apiVersion!=AP::kVersion ||
        event->size<sizeof(AP::AnimationUpdateEvent)) return;
    const auto receiver=FormID(event->receiver);
    if(!receiver) return;
    if(event->ending) {
        std::scoped_lock lock(sMutex);
        sReceivers.erase(receiver);
        return;
    }
    if(event->actorCount>32 || (event->actorCount>0 && !event->actors)) return;
    Receiver snapshot;
    snapshot.id=receiver;
    snapshot.context=static_cast<std::uint32_t>(event->context);
    snapshot.updated=Clock::now();
    snapshot.others.reserve(event->actorCount);
    for(std::uint32_t i=0;i<event->actorCount;++i) {
        auto copy=CopyPartner(event->actors[i]);
        if(copy.partner) snapshot.others.push_back(copy);
    }
    if(event->selfInteraction) {
        snapshot.hasSelf=true;
        snapshot.self=CopyPartner(*event->selfInteraction);
    }
    {
        std::scoped_lock lock(sMutex);
        sReceivers[receiver]=std::move(snapshot);

        if(sReceivers.size()>128) {
            auto oldest=sReceivers.begin();
            for(auto it=sReceivers.begin();it!=sReceivers.end();++it)
                if(it->second.updated<oldest->second.updated) oldest=it;
            sReceivers.erase(oldest);
        }
    }
    const auto count=sEvents.fetch_add(1,std::memory_order_relaxed)+1;
    if(count==1) logger::debug(MOAN_LOG("MOAN_PPA: first-event receiver={:08X} partners={} self={} context=0x{:X}"),
        receiver,event->actorCount,event->selfInteraction!=nullptr,static_cast<std::uint32_t>(event->context));
}
#if !defined(TMS_MOAN_STANDALONE)
void SaveBool(const wchar_t* key,bool value) {
    const auto ini=GetPluginIniPathForLogging().wstring();
    if(!WritePrivateProfileStringW(L"MoanPPA",key,value?L"1":L"0",ini.c_str()))
        logger::warn(MOAN_LOG("MOAN_PPA: failed to write INI bool"));
}
#endif
struct Candidate {
    bool found=false;
    MoanPhysicalPolicy::Role role=MoanPhysicalPolicy::Role::None;
    Interaction contact;
    int ageMs=0;
};
int SiteWeight(MoanPhysicalPolicy::Site s) noexcept {
    switch(s) {
    case MoanPhysicalPolicy::Site::Both:return 3;
    case MoanPhysicalPolicy::Site::Anus:return 2;
    case MoanPhysicalPolicy::Site::Vagina:return 1;
    default:return 0;
    }
}
void Consider(Candidate& best, MoanPhysicalPolicy::Role role,const Interaction& data,
    int ageMs) noexcept {


    if(!MoanPhysicalPolicy::IsSupportedSite(data.site)) return;

    const int score=SiteWeight(data.site)*10+(role==MoanPhysicalPolicy::Role::Receiving ? 2 :
        role==MoanPhysicalPolicy::Role::Self ? 1 : 0);
    const int previous=SiteWeight(best.contact.site)*10+
        (best.role==MoanPhysicalPolicy::Role::Receiving ? 2 : best.role==MoanPhysicalPolicy::Role::Self ? 1 : 0);
    if(!best.found || score>previous || (score==previous && ageMs<best.ageMs))
        best={true,role,data,ageMs};
}
}
void LoadSettings() {
#if defined(TMS_MOAN_STANDALONE)


    sEnabled.store(true);
    sApply.store(true);
#else
    const auto ini=GetPluginIniPathForLogging().wstring();
    sEnabled.store(GetPrivateProfileIntW(L"MoanPPA",L"Enabled",0,ini.c_str())!=0);
    sApply.store(GetPrivateProfileIntW(L"MoanPPA",L"Apply",0,ini.c_str())!=0);
#endif

    sMaxAgeMs.store(2500);
    logger::debug(MOAN_LOG("MOAN_PPA: settings enabled={} apply={} policy=site-only maxAgeMs={} (no PPA hard dependency)"),
        sEnabled.load(),sApply.load(),sMaxAgeMs.load());
}
void Connect() noexcept {
    if(sConnected.load()) return;
    const auto module=GetModuleHandleW(AP::kPluginDLL);
    if(!module) {logger::info(MOAN_LOG("MOAN_PPA: state=unavailable reason=DLLMissing fallback=excitement-only"));return;}
    using QueryFn=const AP::InterfaceV1*(__cdecl*)();
    const auto query=reinterpret_cast<QueryFn>(GetProcAddress(module,AP::kGetAPIFunctionNameV1));
    const auto* api=query ? query() : nullptr;
    if(!api || api->version!=AP::kVersion || api->size<sizeof(AP::InterfaceV1) ||
       !api->RegisterAnimationUpdateListener || !api->UnregisterAnimationUpdateListener) {
        logger::warn(MOAN_LOG("MOAN_PPA: state=unavailable reason=MissingOrMismatchedAPIV1 fallback=excitement-only"));return;
    }
    const auto handle=api->RegisterAnimationUpdateListener(&OnEvent,nullptr);
    if(!handle) {logger::warn(MOAN_LOG("MOAN_PPA: state=unavailable reason=ListenerRejected fallback=excitement-only"));return;}
    sAPI=api;
    sListener=handle;
    sConnected.store(true);
    logger::info(MOAN_LOG("MOAN_PPA: state=connected apiVersion={} listener={} mode={}"),
        api->version,handle,sApply.load()?"apply":"observe");


}
void ResetSnapshots() noexcept {
    std::scoped_lock lock(sMutex);
    sReceivers.clear();
}
bool IsConnected() noexcept {return sConnected.load();}
bool IsReceivingMouthContact(std::uint32_t actorFormID, int* ageMs) noexcept {


    if (ageMs) *ageMs = -1;
    if (!actorFormID || !sEnabled.load(std::memory_order_relaxed) ||
        !sApply.load(std::memory_order_relaxed) ||
        !sConnected.load(std::memory_order_relaxed)) return false;
    std::scoped_lock lock(sMutex);
    const auto it = sReceivers.find(actorFormID);
    if (it == sReceivers.end()) return false;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - it->second.updated).count();


    constexpr std::int64_t kMouthContactMaxAgeMs = 250;
    if (age < 0 || age > kMouthContactMaxAgeMs) return false;
    for (const auto& other : it->second.others) {
        if (!other.partner || other.site != MoanPhysicalPolicy::Site::Mouth ||
            !(other.depth > 0.0f)) continue;
        if (ageMs) *ageMs = static_cast<int>(age);
        return true;
    }
    if (it->second.hasSelf &&
        it->second.self.site == MoanPhysicalPolicy::Site::Mouth &&
        it->second.self.depth > 0.0f) {
        if (ageMs) *ageMs = static_cast<int>(age);
        return true;
    }
    return false;
}
Decision EvaluateActor(std::uint32_t actorFormID,int excitement) noexcept {
    Decision out;
    if(!sEnabled.load() || !sConnected.load() || !actorFormID || excitement<0 || excitement>100) return out;
    Candidate chosen;

    struct RawTrace {
        bool valid=false, hasSelf=false, mergedBoth=false;
        std::uint32_t receiver=0, context=0;
        int ageMs=0;
        std::array<Interaction,32> contacts{};
        std::size_t count=0;
    } raw;
    {
        std::scoped_lock lock(sMutex);
        const auto now=Clock::now();
        const int maxAge=sMaxAgeMs.load();
        for(const auto& [id,recv] : sReceivers) {
            const auto age=std::chrono::duration_cast<std::chrono::milliseconds>(now-recv.updated).count();
            if(age<0 || age>maxAge) continue;
            const auto ageMs=static_cast<int>(age);
            if(id==actorFormID) {




                const bool multiPartnerBoth=PPAContactAggregation::HasDualReceivingSites(recv.others);
                raw.valid=true;
                raw.receiver=id; raw.context=recv.context; raw.ageMs=ageMs;
                raw.hasSelf=recv.hasSelf; raw.mergedBoth=multiPartnerBoth;
                for(const auto& other:recv.others) {
                    if(raw.count>=raw.contacts.size()) break;
                    raw.contacts[raw.count++]=other;
                }
                if(recv.hasSelf) Consider(chosen,MoanPhysicalPolicy::Role::Self,recv.self,ageMs);
                for(const auto& other:recv.others)
                    Consider(chosen,MoanPhysicalPolicy::Role::Receiving,other,ageMs);
                if(multiPartnerBoth) {


                    auto combined=Interaction{};
                    combined.site=MoanPhysicalPolicy::Site::Both;
                    Consider(chosen,MoanPhysicalPolicy::Role::Receiving,combined,ageMs);
                }
            }
            for(const auto& other:recv.others) if(other.partner==actorFormID)
                Consider(chosen,MoanPhysicalPolicy::Role::Penetrating,other,ageMs);
        }
    }
    if(raw.valid) {
        logger::debug(MOAN_LOG("MOAN_PPA_RAW: receiver={:08X} ageMs={} context=0x{:X} ")
            "partners={} self={} mergedBoth={}",
            raw.receiver,raw.ageMs,raw.context,raw.count,raw.hasSelf,raw.mergedBoth);
        for(std::size_t i=0;i<raw.count;++i) {
            const auto& other=raw.contacts[i];
            logger::debug(MOAN_LOG("MOAN_PPA_RAW: receiver={:08X} partner={:08X} ")
                "site={} contactValid={}",
                raw.receiver,other.partner,MoanPhysicalPolicy::SiteName(other.site),
                other.depth>0.0f);
        }
    }
    if(!chosen.found) return out;
    MoanPhysicalPolicy::Input input;
    input.excitement=excitement;
    input.role=chosen.role;
    input.site=chosen.contact.site;
    const auto r=MoanPhysicalPolicy::Evaluate(input);
    out.valid=true;
    out.applied=sApply.load();
    out.role=chosen.role;out.site=chosen.contact.site;
    out.ageMs=chosen.ageMs;
    out.score=r.score;out.floor=r.floor;out.multiplier=r.intervalMultiplier;
    return out;
}
#if !defined(TMS_MOAN_STANDALONE)
void __stdcall RenderMenu() {
    ImGuiMCP::TextWrapped("PPA is optional. Missing or stale contact data falls back to OStim excitement.");
    ImGuiMCP::Text("PPA API: %s",sConnected.load()?"Connected":"Unavailable");
    bool enabled=sEnabled.load();
    if(ImGuiMCP::Checkbox("Enable PPA Data##dm_ppa_enable",&enabled)) {
        sEnabled.store(enabled); SaveBool(L"Enabled",enabled);
        if(!enabled) ResetSnapshots();
    }
    bool apply=sApply.load();
    if(ImGuiMCP::Checkbox("Apply Contact-Site Rules##dm_ppa_apply",&apply)) {
        sApply.store(apply); SaveBool(L"Apply",apply);
    }
    ImGuiMCP::TextWrapped("Vagina / Anus / Both use the established site-based tier and frequency rules. Receiving Mouth passes through native OStim audio.");
    ImGuiMCP::TextDisabled("No size or depth intensity bonuses. No extra sound channel.");
    ImGuiMCP::TextDisabled("Contact snapshot timeout: 2500 ms (internal). Mouth validity: 250 ms.");
}
#endif
}
