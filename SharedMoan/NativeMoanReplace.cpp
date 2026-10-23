#include "PCH.h"
#include "NativeMoanReplace.h"
#include "MoanTierPolicy.h"
#include "ClimaxMoanPolicy.h"
#include "MoanFrequencyPolicy.h"
#include "PPAEnhancement.h"
#if defined(TMS_MOAN_STANDALONE)
#include "MoanWavDuration.h"
#else
#include "core/DialogueVoiceTimerGuard.h"
#include "DialogueVoiceCache.h"
#include "SKSEMenuFramework.h"
#include "common/localization/DDFLocalization.h"
#endif
#include "RuntimePaths.h"
#if defined(TMS_MOAN_STANDALONE)
#include "SKSEMenuFramework.h"
#endif
#include "core/RuntimeHelpers.h"
#include "RE/B/BSAudioManager.h"
#include "RE/B/BSSoundHandle.h"
#include "RE/I/ID.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cwchar>
#include <exception>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cmath>
#include <fmt/format.h>
#include <random>
#include <string>
#include <string_view>
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
namespace TMS::DD::NativeMoanReplace {
namespace {
    namespace fs = std::filesystem;
    using namespace MoanTierPolicy;
    enum class Mode : std::uint8_t { Single, Tiers };
    std::atomic_bool sEnabled{false};
    std::atomic_bool sCallHookReady{false};
    std::atomic_uint32_t sFormID{0};
    std::atomic_uint32_t sReplaced{0};
    Mode sMode = Mode::Single;
    struct MoanFile { std::string path; float durationSeconds = 0.0f; };
    MoanFile sSingleFile;


    std::array<std::vector<MoanFile>, 3> sTierFiles;
    std::vector<MoanFile> sClimaxFiles;
#if defined(TMS_MOAN_STANDALONE)
    std::array<std::vector<MoanFile>, 3> sMaleTierFiles;
    std::vector<MoanFile> sMaleClimaxFiles;

    int SoundGroupForActor(RE::Actor* actor) noexcept {
        if (!actor) return -1;
        auto* base = actor->GetActorBase();
        if (!base) return -1;
        switch (base->GetSex()) {
        case RE::SEXES::kFemale: return 0;
        case RE::SEXES::kMale: return 1;
        default: return -1;
        }
    }
#endif
    std::atomic_bool sClimaxEnabled{false};
    std::atomic_bool sClimaxHookInstalled{false};

    std::atomic_bool sFrequencyEnabled{false};
    std::atomic_bool sVoiceGuardPriority{false};
#if defined(TMS_MOAN_STANDALONE)
    constexpr const char* kAudioPrefix = "fx/TMSDynamicMoans/Moans/";
    constexpr const wchar_t* kAudioDirectory = L"TMSDynamicMoans";
#else
    constexpr const char* kAudioPrefix = "fx/TMSDynamicDialogue/Moans/";
    constexpr const wchar_t* kAudioDirectory = L"TMSDynamicDialogue";
#endif
    std::array<std::atomic_bool,3> sTierFrequencyEnabled{{true,true,true}};
#if defined(TMS_MOAN_STANDALONE)
    std::array<std::atomic<float>,3> sTierIntervalSeconds{{4.0f,3.0f,2.0f}};
#else
    std::array<std::atomic<float>,3> sTierIntervalSeconds{{6.0f,4.5f,3.0f}};
#endif
    std::atomic<float> sSafetyGapSeconds{0.40f};
    RE::TESFaction* sExcitementFaction = nullptr;
    int sLowMax = 33;
    int sMediumMax = 66;
    int sForcedExcitement = -1;
    std::atomic<NativeBuildFn> sOriginal{nullptr};

    struct Context {
        bool active=false, consumed=false, replaced=false;
        const MoanFile* file=nullptr;
    };
    thread_local Context tContext;

    bool ParseID(std::wstring_view value, std::uint32_t& out) noexcept {
        if(value.starts_with(L"0x") || value.starts_with(L"0X")) value.remove_prefix(2);
        if(value.empty() || value.size()>8) return false;
        std::uint32_t v=0;
        for(auto c:value) {
            unsigned x;
            if(c>=L'0'&&c<=L'9') x=c-L'0';
            else if(c>=L'a'&&c<=L'f') x=c-L'a'+10;
            else if(c>=L'A'&&c<=L'F') x=c-L'A'+10;
            else return false;
            v=(v<<4)|x;
        }
        if(!v) return false;
        out=v; return true;
    }
    bool SafeRelative(std::string_view s) noexcept {
        if(s.empty() || s.size()>220 || s.starts_with('/') || s.find("..")!=s.npos) return false;
        if(s.size()<5 || s.substr(s.size()-4)!=".wav") return false;
        for(unsigned char c:s) {
            if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
                (c>='0'&&c<='9') || c=='_' || c=='-' || c=='/' || c=='.')) return false;
        }
        return true;
    }
    float CachedDuration(const std::string& relative) {
#if defined(TMS_MOAN_STANDALONE)

        const float duration = MoanWavDuration::ReadDuration(
            GetGameDirectory() / "Data" / "Sound" / std::filesystem::path(relative));
        if (duration > 0.0f) return duration;
        logger::warn("DM_MOAN_FREQ: duration=unavailable file='{}' action=KeepNativeCooldown", relative);
        return 0.0f;
#else

        const auto asset=DialogueVoiceCache::GetSingleton().Resolve(relative);
        if (asset.ready && std::isfinite(asset.durationSeconds) &&
            asset.durationSeconds>0.0f && asset.durationSeconds<=300.0f)
            return asset.durationSeconds;
        logger::warn(MOAN_LOG("MOAN_FREQ: duration=unavailable file='{}' reason='{}' action=KeepNativeCooldown"),
            relative,asset.error);
        return 0.0f;
#endif
    }
    bool ValidWav(const fs::path& file) {
        std::error_code ec;
        if(!fs::is_regular_file(file,ec)) return false;
        const auto size=fs::file_size(file,ec);
        if(ec || size<44 || size>64ull*1024*1024) return false;
        std::ifstream in(file,std::ios::binary);
        std::array<char,12> header{};
        return static_cast<bool>(in.read(header.data(),header.size())) &&
            std::memcmp(header.data(),"RIFF",4)==0 &&
            std::memcmp(header.data()+8,"WAVE",4)==0;
    }
    void GenerateID(RE::BSResource::ID& id, const char* str) {
        using fn=void(RE::BSResource::ID*,const char*);
        static REL::Relocation<fn> f{REL::RelocationID(68635,69979)};
        f(&id,str);
    }
    void BuildFile(RE::BSAudioManager* mgr, RE::BSSoundHandle& handle,
        const RE::BSResource::ID& id) {
        using fn=void(RE::BSAudioManager*,RE::BSSoundHandle&,
            const RE::BSResource::ID&,std::uint32_t,std::uint32_t);
        static REL::Relocation<fn> f{REL::RelocationID(66402,67664)};
        f(mgr,handle,id,0x1A,1);
    }
    bool BuildReplacement(RE::BSAudioManager* mgr,RE::BSSoundHandle& handle,
        const std::string& path) {
        for(const auto& candidate: {"Sound/"+path,path}) {
            RE::BSResource::ID id{};
            GenerateID(id,candidate.c_str());
            const bool wasValid=handle.IsValid();
            const auto oldSoundID=handle.soundID;
            BuildFile(mgr,handle,id);
            if(handle.IsValid() && (!wasValid || handle.soundID!=oldSoundID)) return true;
        }
        return false;
    }

    void DiscoverTier(const fs::path& root, std::string_view group, int i) {
#if defined(TMS_MOAN_STANDALONE)
        auto& files = group=="Male" ? sMaleTierFiles[i] : sTierFiles[i];
#else
        auto& files = sTierFiles[i];
#endif
        const char* name=Name(static_cast<Tier>(i));
        const fs::path folder=root / fs::path(group) / name;
        std::error_code ec;
        fs::directory_iterator iter(folder,fs::directory_options::skip_permission_denied,ec);
        if(ec) {
            logger::debug(MOAN_LOG("NATIVE_MOAN: scan group={} tier={} files=0 reason=FolderMissing"),group,name);
            return;
        }
        unsigned ignored=0;
        const fs::directory_iterator end{};
        for(;iter!=end;iter.increment(ec)) {
            if(ec) {logger::warn(MOAN_LOG("NATIVE_MOAN: scan group={} tier={} reason=EnumerationError"),group,name);break;}
            if(files.size()>=256) { ++ignored; continue; }
            const auto filename=iter->path().filename().string();
            const std::string relative=std::string(group)+"/"+name+"/"+filename;
            if(!SafeRelative(relative) || !ValidWav(iter->path())) { ++ignored; continue; }
            const std::string path=std::string(kAudioPrefix)+relative;
            files.push_back({path,CachedDuration(path)});
        }
        std::sort(files.begin(),files.end(),
            [](const MoanFile& a,const MoanFile& b){return a.path<b.path;});
        logger::debug(MOAN_LOG("NATIVE_MOAN: scan group={} tier={} files={} ignored={}"),
            group,name,files.size(),ignored);
    }
    void DiscoverClimax(const fs::path& root, std::string_view group) {
#if defined(TMS_MOAN_STANDALONE)
        auto& files = group=="Male" ? sMaleClimaxFiles : sClimaxFiles;
#else
        auto& files = sClimaxFiles;
#endif
        const auto folder=root / fs::path(group) / "Climax";
        std::error_code ec;
        fs::directory_iterator iter(folder,fs::directory_options::skip_permission_denied,ec);
        if(ec) {
            logger::debug(MOAN_LOG("MOAN_CLIMAX: state=files-missing group={} action=NativeFallback"),group);
            return;
        }
        unsigned ignored=0;
        for(const fs::directory_iterator end{};iter!=end;iter.increment(ec)) {
            if(ec) break;
            if(files.size()>=256) {++ignored;continue;}
            const std::string filename=iter->path().filename().string();
            const std::string relative=std::string(group)+"/Climax/"+filename;
            if(!SafeRelative(relative) || !ValidWav(iter->path())) {++ignored;continue;}
            const std::string path=std::string(kAudioPrefix)+relative;
            files.push_back({path,CachedDuration(path)});
        }
        std::sort(files.begin(),files.end(),
            [](const MoanFile& x,const MoanFile& y){return x.path<y.path;});
        logger::debug(MOAN_LOG("MOAN_CLIMAX: group={} files={} ignored={}"),group,files.size(),ignored);
    }
    const MoanFile* RandomFromFiles(const std::vector<MoanFile>& files) noexcept {
        if(files.empty()) return nullptr;
        thread_local std::mt19937 engine([] {
            const auto tick=static_cast<std::uint32_t>(GetTickCount64());
            return std::mt19937(tick ^ (GetCurrentThreadId()*0x9E3779B9u) ^ 0xA7E9B421u);
        }());
        std::uniform_int_distribution<std::size_t> pick(0,files.size()-1);
        return &files[pick(engine)];
    }
    const MoanFile* RandomFile(int tier, bool male=false) noexcept {
        if (tier<0 || tier>2) return nullptr;
#if defined(TMS_MOAN_STANDALONE)
        return RandomFromFiles(male ? sMaleTierFiles[tier] : sTierFiles[tier]);
#else
        (void)male;
        return RandomFromFiles(sTierFiles[tier]);
#endif
    }

}
namespace {
#if !defined(TMS_MOAN_STANDALONE)
    float IniFloat(const std::wstring& ini,const wchar_t* key,float fallback,float minValue,float maxValue) {
        wchar_t buffer[48]{};
        wchar_t fallbackText[32]{};
        std::swprintf(fallbackText,std::size(fallbackText),L"%.2f",fallback);
        GetPrivateProfileStringW(L"MoanFrequency",key,fallbackText,buffer,
            static_cast<DWORD>(std::size(buffer)),ini.c_str());
        wchar_t* end=nullptr;
        const float result=std::wcstof(buffer,&end);
        return (end!=buffer && *end==0 && std::isfinite(result)) ?
            std::clamp(result,minValue,maxValue) : fallback;
    }
    void LoadFrequency(const std::wstring& ini) {
        sFrequencyEnabled.store(GetPrivateProfileIntW(L"MoanFrequency",L"Enabled",0,ini.c_str())!=0);
        #if defined(TMS_MOAN_STANDALONE)
        sVoiceGuardPriority.store(false);
#else
        sVoiceGuardPriority.store(GetPrivateProfileIntW(L"MoanFrequency",L"VoiceGuardPriority",0,ini.c_str())!=0);
#endif
        constexpr const wchar_t* enabledNames[]={L"LowEnabled",L"MediumEnabled",L"HighEnabled"};
        constexpr const wchar_t* intervalNames[]={L"LowIntervalSeconds",L"MediumIntervalSeconds",L"HighIntervalSeconds"};
        constexpr float defaults[]={6.0f,4.5f,3.0f};
        for(int i=0;i<3;++i) {
            sTierFrequencyEnabled[i].store(GetPrivateProfileIntW(L"MoanFrequency",enabledNames[i],1,ini.c_str())!=0);
            sTierIntervalSeconds[i].store(IniFloat(ini,intervalNames[i],defaults[i],1.0f,15.0f));
        }
        sSafetyGapSeconds.store(IniFloat(ini,L"SafetyGapSeconds",0.4f,0.2f,2.0f));
        logger::info(MOAN_LOG("MOAN_FREQ: state=loaded enabled={} low={:.2f}s medium={:.2f}s high={:.2f}s gap={:.2f}s perTier={}/{}/{}"),
            sFrequencyEnabled.load(),sTierIntervalSeconds[0].load(),sTierIntervalSeconds[1].load(),
            sTierIntervalSeconds[2].load(),sSafetyGapSeconds.load(),
            sTierFrequencyEnabled[0].load(),sTierFrequencyEnabled[1].load(),sTierFrequencyEnabled[2].load());
    }
#else


    void LoadFrequency(const std::wstring&) {
        sFrequencyEnabled.store(true);
        sVoiceGuardPriority.store(false);
        for (auto& on : sTierFrequencyEnabled) on.store(true);
        sTierIntervalSeconds[0].store(4.0f);
        sTierIntervalSeconds[1].store(3.0f);
        sTierIntervalSeconds[2].store(2.0f);
        sSafetyGapSeconds.store(0.4f);
        logger::debug("DM_MOAN_FREQ: state=fixed enabled=true low=4.00s medium=3.00s high=2.00s gap=0.40s");
    }
#endif
#if !defined(TMS_MOAN_STANDALONE)
    void SaveFrequency(const wchar_t* key,const std::wstring& value) {
        const auto ini=GetPluginIniPathForLogging().wstring();
        if (!WritePrivateProfileStringW(L"MoanFrequency",key,value.c_str(),ini.c_str())) {
            std::string keyText;
            if (key) {
                for (const wchar_t* c = key; *c; ++c)
                    keyText.push_back(*c <= 127 ? static_cast<char>(*c) : '?');
            }
            logger::warn(MOAN_LOG("MOAN_FREQ: INI write failed key='{}'"), keyText);
        }
    }
    void SaveFrequencyBool(const wchar_t* key,bool val) {SaveFrequency(key,val?L"1":L"0");}
    void SaveFrequencyFloat(const wchar_t* key,float val) {
        wchar_t buffer[32]{};
        std::swprintf(buffer,std::size(buffer),L"%.2f",val);
        SaveFrequency(key,buffer);
    }
#endif
}
void LoadSettings() {
    PPAEnhancement::LoadSettings();
    LoadFrequency(GetPluginIniPathForLogging().wstring());
    sEnabled.store(false,std::memory_order_release);
    sCallHookReady.store(false,std::memory_order_release);
    sFormID.store(0,std::memory_order_release);
    sReplaced.store(0,std::memory_order_relaxed);
    sSingleFile={};
    for(auto& tier:sTierFiles) tier.clear();
    sClimaxFiles.clear();
#if defined(TMS_MOAN_STANDALONE)
    for(auto& tier:sMaleTierFiles) tier.clear();
    sMaleClimaxFiles.clear();
#endif
    sClimaxEnabled.store(false,std::memory_order_release);
    sClimaxHookInstalled.store(false,std::memory_order_release);
    sExcitementFaction=nullptr;
    sForcedExcitement=-1;
    sMode=Mode::Single;
    try {
        const auto ini=GetPluginIniPathForLogging().wstring();
        if(GetPrivateProfileIntW(L"NativeMoanReplace",L"Enabled",
#if defined(TMS_MOAN_STANDALONE)
            1,
#else
            0,
#endif
            ini.c_str())==0){
            logger::info(MOAN_LOG("NATIVE_MOAN: state=disabled action=PASS")); return;
        }
#if defined(TMS_MOAN_STANDALONE)


        constexpr std::uint32_t actorID = 0x14u;
        sMode = Mode::Tiers;
#else
        wchar_t actorText[32]{};
        wchar_t modeText[32]{};
        GetPrivateProfileStringW(L"NativeMoanReplace",L"ActorFormID",L"",
            actorText,static_cast<DWORD>(std::size(actorText)),ini.c_str());
        GetPrivateProfileStringW(L"NativeMoanReplace",L"Mode",L"single",
            modeText,static_cast<DWORD>(std::size(modeText)),ini.c_str());
        std::uint32_t actorID{};
        if(!ParseID(actorText,actorID)) {
            logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=InvalidActorID action=PASS"));return;
        }
        if(_wcsicmp(modeText,L"tiers")==0) sMode=Mode::Tiers;
        else if(_wcsicmp(modeText,L"single")!=0) {
            logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=UnknownMode action=PASS"));return;
        }
#endif
        const fs::path root=GetGameDirectory() / "Data" / "Sound" / "fx" / kAudioDirectory / "Moans";
        if(sMode==Mode::Single) {
            wchar_t fileText[256]{};
            GetPrivateProfileStringW(L"NativeMoanReplace",L"File",L"Female/Low/mild_moans_01.wav",
                fileText,static_cast<DWORD>(std::size(fileText)),ini.c_str());
            std::string relative;
            for(wchar_t c:fileText) {
                if(c==0) break;
                if(c>127) {logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=NonAsciiFile action=PASS"));return;}
                relative.push_back(c==L'\\' ? '/' : static_cast<char>(c));
            }
            if(!SafeRelative(relative) || !ValidWav(root / fs::path(relative))) {
                logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=MissingOrInvalidSingleFile action=PASS"));return;
            }
            sSingleFile.path=std::string(kAudioPrefix)+relative;
            sSingleFile.durationSeconds=CachedDuration(sSingleFile.path);
        } else {
#if defined(TMS_MOAN_STANDALONE)


            sLowMax=33;
            sMediumMax=66;
#else
            wchar_t groupText[16]{};
            GetPrivateProfileStringW(L"NativeMoanReplace",L"Group",L"Female",
                groupText,static_cast<DWORD>(std::size(groupText)),ini.c_str());
            const std::string group=(_wcsicmp(groupText,L"Female")==0) ? "Female" :
                                    (_wcsicmp(groupText,L"Male")==0) ? "Male" : "";
            if(group.empty()) {logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=InvalidGroup action=PASS"));return;}
            sLowMax=GetPrivateProfileIntW(L"NativeMoanReplace",L"LowMax",33,ini.c_str());
            sMediumMax=GetPrivateProfileIntW(L"NativeMoanReplace",L"MediumMax",66,ini.c_str());
            #if !defined(TMS_MOAN_STANDALONE)
            sForcedExcitement=GetPrivateProfileIntW(L"NativeMoanReplace",L"ForcedExcitement",-1,ini.c_str());
#endif
#endif
            if(sLowMax<0 || sLowMax>=sMediumMax || sMediumMax>=100 ||
                sForcedExcitement < -1 || sForcedExcitement > 100) {
                logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=InvalidThresholdOrOverride action=PASS"));return;
            }
            if(sForcedExcitement<0) {
                if(auto* data=RE::TESDataHandler::GetSingleton()) {
                    sExcitementFaction=data->LookupForm<RE::TESFaction>(0xD93,"OStim.esp");
                }
                if(!sExcitementFaction) {
                    logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=OStimExcitementFactionMissing action=PASS"));return;
                }
            }
#if defined(TMS_MOAN_STANDALONE)
            for (const auto* group : {"Female", "Male"}) {
                for(int i=0;i<3;++i) DiscoverTier(root,group,i);
                DiscoverClimax(root,group);
            }
            sClimaxEnabled.store(true,std::memory_order_release);
            const auto femaleCount=sTierFiles[0].size()+sTierFiles[1].size()+sTierFiles[2].size();
            const auto maleCount=sMaleTierFiles[0].size()+sMaleTierFiles[1].size()+sMaleTierFiles[2].size();
            if(!femaleCount && !maleCount) {
                logger::warn("DM_NATIVE_MOAN: state=inactive reason=NoValidTierWav action=PASS");return;
            }
            if(!femaleCount) logger::warn("DM_NATIVE_MOAN: group=Female action=NativeFallback reason=NoTierWav");
            if(!maleCount) logger::warn("DM_NATIVE_MOAN: group=Male action=NativeFallback reason=NoTierWav");
            logger::info("DM_LIBRARY: Female={}/{}/{}/{} Male={}/{}/{}/{} (Low/Medium/High/Climax)",
                sTierFiles[0].size(),sTierFiles[1].size(),sTierFiles[2].size(),sClimaxFiles.size(),
                sMaleTierFiles[0].size(),sMaleTierFiles[1].size(),sMaleTierFiles[2].size(),sMaleClimaxFiles.size());
#else
            for(int i=0;i<3;++i) DiscoverTier(root,group,i);
            DiscoverClimax(root,group);
            sClimaxEnabled.store(
                GetPrivateProfileIntW(L"NativeMoanReplace",L"ClimaxEnabled",0,ini.c_str())!=0,
                std::memory_order_release);
            const auto total=sTierFiles[0].size()+sTierFiles[1].size()+sTierFiles[2].size();
            if(!total) {
                logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=NoValidTierWav action=PASS"));return;
            }
            logger::info(MOAN_LOG("NATIVE_MOAN: tiers group={} lowMax={} mediumMax={} ")
                "forcedExcitement={} counts={}/{}/{} source={} fallback=nearest-lower-first",
                group,sLowMax,sMediumMax,sForcedExcitement,
                sTierFiles[0].size(),sTierFiles[1].size(),sTierFiles[2].size(),
                sForcedExcitement>=0?"ini-override":"OStimExcitementFaction");
#endif
        }
        sFormID.store(actorID,std::memory_order_relaxed);
        sEnabled.store(true,std::memory_order_release);
        logger::debug(MOAN_LOG("NATIVE_MOAN: state=ready actor={:08X} mode={} ")
            "route=OStim-GameSound noESP=true noSpeakSound=true nativeFallback=PASS",
            actorID,sMode==Mode::Tiers?"tiers":"single");
    } catch(const std::exception& e) {
        logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=SetupException what='{}' action=PASS"),e.what());
    } catch(...) {
        logger::warn(MOAN_LOG("NATIVE_MOAN: state=inactive reason=SetupException action=PASS"));
    }
}
bool IsEnabled() noexcept {return sEnabled.load(std::memory_order_acquire);}
void SetNativeCallHookInstalled(bool ready) noexcept {sCallHookReady.store(ready,std::memory_order_release);}
bool IsReady() noexcept {return IsEnabled() && sCallHookReady.load(std::memory_order_acquire);}
bool ShouldSuppressForVoiceGuard(std::uint32_t actorFormID) noexcept {
#if defined(TMS_MOAN_STANDALONE)
    (void)actorFormID;
    return false;
#else
    return sVoiceGuardPriority.load(std::memory_order_relaxed) && IsReady() &&
        actorFormID==sFormID.load(std::memory_order_relaxed) &&
        TMS::DD::DialogueVoiceTimerGuard::GetSingleton().IsActorGuardActive(actorFormID);
#endif
}
void SetOriginalBuildFn(NativeBuildFn fn) noexcept {sOriginal.store(fn,std::memory_order_release);}
bool IsClimaxEnabled() noexcept { return sClimaxEnabled.load(std::memory_order_acquire); }
void SetClimaxEnabled(bool value) noexcept {
    sClimaxEnabled.store(value,std::memory_order_release);
}
void SetClimaxHookInstalled(bool ready) noexcept {
    sClimaxHookInstalled.store(ready,std::memory_order_release);
}
bool IsClimaxHookAvailable() noexcept {
#if defined(TMS_MOAN_STANDALONE)
    return IsReady() && sClimaxHookInstalled.load(std::memory_order_acquire) &&
        (!sClimaxFiles.empty() || !sMaleClimaxFiles.empty());
#else
    return IsReady() && sClimaxHookInstalled.load(std::memory_order_acquire) && !sClimaxFiles.empty();
#endif
}
bool HasClimaxFiles() noexcept {
#if defined(TMS_MOAN_STANDALONE)
    return IsReady() && sMode==Mode::Tiers &&
        (!sClimaxFiles.empty() || !sMaleClimaxFiles.empty());
#else
    return IsReady() && sMode==Mode::Tiers && !sClimaxFiles.empty();
#endif
}
ClimaxScope::ClimaxScope(std::uint32_t actorFormID) noexcept : actorFormID_(actorFormID) {
    previousActive_=tContext.active;
    previousConsumed_=tContext.consumed;
    previousReplaced_=tContext.replaced;
    previousFile_=tContext.file;
    const bool matched=actorFormID_!=0 &&
        actorFormID_==sFormID.load(std::memory_order_relaxed);
#if defined(TMS_MOAN_STANDALONE)

    auto* player=matched ? RE::PlayerCharacter::GetSingleton() : nullptr;
    const int group=SoundGroupForActor(player);
    const auto* climaxFiles=group==0 ? &sClimaxFiles :
                            group==1 ? &sMaleClimaxFiles : nullptr;
    const bool filesReady=HasClimaxFiles() && climaxFiles && !climaxFiles->empty();
#else
    const bool filesReady=HasClimaxFiles();
#endif
#if defined(TMS_MOAN_STANDALONE)
    const bool guardActive=false;
#else
    const bool guardActive=filesReady && matched &&
        TMS::DD::DialogueVoiceTimerGuard::GetSingleton().IsActorGuardActive(actorFormID_);
#endif
    const auto decision=ClimaxMoanPolicy::Decide(IsReady(),filesReady,
        sClimaxEnabled.load(std::memory_order_relaxed),matched,guardActive);
    selected_=decision!=ClimaxMoanPolicy::Decision::Pass;
    if(!selected_) return;


    if(decision==ClimaxMoanPolicy::Decision::Suppress) {
        suppressed_=true;
        reason_="VoiceGuardActive";
        return;
    }
#if defined(TMS_MOAN_STANDALONE)
    if(const auto* file=RandomFromFiles(*climaxFiles)) {
#else
    if(const auto* file=RandomFromFiles(sClimaxFiles)) {
#endif
        reason_="Selected";
        tContext={true,false,false,file};
    } else reason_="NoWav";
}
ClimaxScope::~ClimaxScope() noexcept {
    if(!selected_) return;
    try {
        MOAN_EVENT(MOAN_LOG("MOAN_CLIMAX: actor={:08X} decision={} reason={} file='{}' ")
            "buildAttempted={} replaced={}",actorFormID_,
            suppressed_?"SUPPRESS":(tContext.replaced?"REPLACE":"PASS"),reason_,
            (!suppressed_ && tContext.file)?tContext.file->path:"<native>",
            !suppressed_ && tContext.consumed,!suppressed_ && tContext.replaced);
#if defined(TMS_MOAN_STANDALONE)
        if(!suppressed_ && tContext.replaced) {
            static std::atomic_bool loggedFirst{false};
            bool expected=false;
            if(loggedFirst.compare_exchange_strong(expected,true))
                logger::debug("DM_MOAN_CLIMAX: first-replacement actor={:08X} decision=REPLACE",actorFormID_);
        }
#endif
    } catch(...) {}
    tContext={previousActive_,previousConsumed_,previousReplaced_,static_cast<const MoanFile*>(previousFile_)};
}
OrdinaryScope::OrdinaryScope(std::uint32_t actorFormID, RE::Actor* actor, RE::Actor* partnerActor) noexcept {
    previousActive_=tContext.active;
    previousConsumed_=tContext.consumed;
    previousReplaced_=tContext.replaced;
    previousFile_=tContext.file;
    actorFormID_=actorFormID;
    selected_=IsReady() && actorFormID==sFormID.load(std::memory_order_relaxed);
    if(!selected_) return;




    int mouthAgeMs=-1;
    if(PPAEnhancement::IsReceivingMouthContact(actorFormID_, &mouthAgeMs)) {
        reason_="MouthOccupied_NativeOStim";
        if(sMode==Mode::Tiers) {
            MOAN_EVENT(MOAN_LOG("MOAN_MOUTH: actor={:08X} occupied=true ageMs={} ")
                "decision=PASS route=OStimNative reason=PPAReceiverMouth",
                actorFormID_,mouthAgeMs);
        }
        return;
    }
    if(sMode==Mode::Single) {
        reason_="SingleFile";
        tContext={true,false,false,&sSingleFile};
        return;
    }
    if(!actor) {reason_="ActorMissing";return;}
#if defined(TMS_MOAN_STANDALONE)
    const int group=SoundGroupForActor(actor);
    if(group<0) {reason_="ActorSexUnknown_NativeFallback";return;}
    maleLibrary_=(group==1);
    static std::array<std::atomic_bool,2> loggedSex{};
    if(!loggedSex[static_cast<std::size_t>(group)].exchange(true))
        logger::info("DM_NATIVE_MOAN: player sex={} library={} source=ActorBase::GetSex",
            maleLibrary_?"Male":"Female",maleLibrary_?"Male":"Female");
#endif
    if(sForcedExcitement>=0) excitement_=sForcedExcitement;
    else {
        if(!sExcitementFaction) {reason_="FactionMissing";return;}
        const bool isPlayer = actor == RE::PlayerCharacter::GetSingleton();
        excitement_ = actor->GetFactionRank(sExcitementFaction,isPlayer);
    }
    baseTier_=MoanTierPolicy::Classify(excitement_,sLowMax,sMediumMax);
    tierRequested_=baseTier_;
    const auto physical=PPAEnhancement::EvaluateActor(actorFormID_,excitement_);
    if(physical.valid) {
        const int predictedTier=MoanTierPolicy::Classify(physical.score,sLowMax,sMediumMax);
        try {
            MOAN_EVENT(MOAN_LOG("MOAN_PPA: actor={:08X} ageMs={} role={} site={} ")
                "excitement={} floor={} score={} baseTier={} ppaTier={} multiplier={:.2f} decision={}",
                actorFormID_,physical.ageMs,
                MoanPhysicalPolicy::RoleName(physical.role),MoanPhysicalPolicy::SiteName(physical.site),
                excitement_,physical.floor,physical.score,baseTier_,predictedTier,
                physical.multiplier,physical.applied?"APPLY":"OBSERVE");
        } catch(...) {}
        if(physical.applied && predictedTier>=0) {
            tierRequested_=predictedTier;
            ppaFrequencyMultiplier_=physical.multiplier;
            ppaApplied_=true;
        }
    } else if(PPAEnhancement::IsConnected()) {


        logger::debug(MOAN_LOG("MOAN_PPA: actor={:08X} decision=DEFAULT reason=NoActivePhysicalSnapshot"),actorFormID_);
    }
    if(tierRequested_<0) {reason_="InvalidExcitement";return;}
#if defined(TMS_MOAN_STANDALONE)




    creaturePartner_ = partnerActor && partnerActor != actor &&
        !partnerActor->HasKeywordString("ActorTypeNPC");
    if (creaturePartner_) {
        const int before = tierRequested_;
        tierRequested_ = CreatureBoost(before, true);
        creatureBoostApplied_ = tierRequested_ != before;
        MOAN_EVENT("DM_MOAN_CREATURE: partner={:08X} before={} after={} capped={} decision={}",
            partnerActor->GetFormID(), Name(static_cast<Tier>(before)),
            Name(static_cast<Tier>(tierRequested_)), !creatureBoostApplied_,
            creatureBoostApplied_ ? "BOOST" : "KEEP_HIGH");
    }
#endif
#if defined(TMS_MOAN_STANDALONE)
    const auto& activeFiles=maleLibrary_ ? sMaleTierFiles : sTierFiles;
#else
    const auto& activeFiles=sTierFiles;
#endif
    const std::array<unsigned,3> counts{
        static_cast<unsigned>(activeFiles[0].size()),
        static_cast<unsigned>(activeFiles[1].size()),
        static_cast<unsigned>(activeFiles[2].size())};
    tierUsed_=MoanTierPolicy::Fallback(tierRequested_,counts);
    if(tierUsed_<0) {reason_="NoTierFile";return;}
    fallback_ = tierUsed_!=tierRequested_;
#if defined(TMS_MOAN_STANDALONE)
    if(const auto* chosen=RandomFile(tierUsed_,maleLibrary_)) {
#else
    if(const auto* chosen=RandomFile(tierUsed_)) {
#endif
        reason_="Selected";
        tContext={true,false,false,chosen};
    } else reason_="RandomSelectionUnavailable";
}
OrdinaryScope::~OrdinaryScope() noexcept {
    if(!selected_) return;
    try {
        if(sMode==Mode::Tiers) {
            MOAN_EVENT(MOAN_LOG("NATIVE_MOAN: actor={:08X} excitement={} requested={} ")
                "selected={} fallback={} file='{}' buildAttempted={} replaced={} decision={} reason={}",
                actorFormID_,excitement_,tierRequested_<0?"Unknown":Name(static_cast<Tier>(tierRequested_)),
                tierUsed_<0?"None":Name(static_cast<Tier>(tierUsed_)),fallback_,
                tContext.active && tContext.file?tContext.file->path:std::string("<native>"),
                tContext.consumed,tContext.replaced,
                tContext.replaced?"REPLACE":"PASS",reason_);
#if defined(TMS_MOAN_STANDALONE)
            if (tContext.replaced && tierUsed_ >= 0 && tierUsed_ < 3) {
                static std::array<std::atomic_bool,3> firstTierSuccess{};
                bool expected = false;
                if(firstTierSuccess[static_cast<std::size_t>(tierUsed_)].compare_exchange_strong(expected, true))
                    logger::debug("DM_NATIVE_MOAN: first-replacement tier={} actor={:08X} decision=REPLACE",
                        Name(static_cast<Tier>(tierUsed_)), actorFormID_);
            }
#endif
        } else {
            MOAN_EVENT(MOAN_LOG("NATIVE_MOAN: actor={:08X} mode=single ")
                "buildAttempted={} replaced={} decision={} route=OStim-GameSound",
                actorFormID_,tContext.consumed,tContext.replaced,
                tContext.replaced?"REPLACE":"PASS");
        }
    } catch(...) {}
    tContext={previousActive_,previousConsumed_,previousReplaced_,static_cast<const MoanFile*>(previousFile_)};
}
void OrdinaryScope::AdjustNativeCooldown(void* timerAddress) const noexcept {
    if(!selected_ || sMode!=Mode::Tiers || tierUsed_<0 || tierUsed_>2 ||
        !timerAddress || !tContext.replaced || !tContext.file ||
        !sFrequencyEnabled.load(std::memory_order_relaxed)) return;


#if defined(TMS_MOAN_STANDALONE)

    const int frequencyTier=(ppaApplied_ || creaturePartner_) && baseTier_>=0
        ? baseTier_ : tierUsed_;
#else
    const int frequencyTier=ppaApplied_ && baseTier_>=0 ? baseTier_ : tierUsed_;
#endif
    if(!sTierFrequencyEnabled[frequencyTier].load(std::memory_order_relaxed)) return;
    const float wavSeconds=tContext.file->durationSeconds;
    if(!std::isfinite(wavSeconds) || wavSeconds<=0.0f) return;
    std::int32_t nativeMs=0;
    std::memcpy(&nativeMs,timerAddress,sizeof(nativeMs));

    if(nativeMs<=0 || nativeMs>120000) return;
    const float baseInterval=sTierIntervalSeconds[frequencyTier].load(std::memory_order_relaxed);
    const float requested=std::clamp(baseInterval*(ppaApplied_ ? ppaFrequencyMultiplier_ : 1.0f),1.0f,15.0f);
    const float gap=sSafetyGapSeconds.load(std::memory_order_relaxed);
    const auto newMs=MoanFrequencyPolicy::NextCooldownMs(requested,wavSeconds,gap);
    if(newMs<=0) return;
    std::memcpy(timerAddress,&newMs,sizeof(newMs));
    try {
        MOAN_EVENT(MOAN_LOG("MOAN_FREQ: actor={:08X} excitement={} tier={} ")
            "baseFrequencyTier={} ppaMultiplier={:.2f} wavSeconds={:.3f} nativeMs={} overrideMs={} intervalSeconds={:.2f} gapSeconds={:.2f} decision=APPLY",
            actorFormID_,excitement_,Name(static_cast<Tier>(tierUsed_)),
            Name(static_cast<Tier>(frequencyTier)),ppaFrequencyMultiplier_,wavSeconds,
            nativeMs,newMs,requested,gap);
    } catch(...) {}
}

#if !defined(TMS_MOAN_STANDALONE)
void __stdcall RenderFrequencyPage() {
    using TMS::DD::Localization::Tr;
    ImGuiMCP::TextWrapped("%s",Tr("$TMS_DDF_MoanFreqHelp",
        "OStim-only ordinary moan frequency. OStim continues to play the sound; the next native cooldown is modified only after a DDF WAV replacement succeeds.").c_str());
    bool enabled=sFrequencyEnabled.load();
    if(ImGuiMCP::Checkbox(TMS::DD::Localization::ImGuiLabel("$TMS_DDF_MoanFreqEnable",
        "Enable Dynamic Moan Frequency").c_str(),&enabled)) {
        sFrequencyEnabled.store(enabled);
        SaveFrequencyBool(L"Enabled",enabled);
        logger::info(MOAN_LOG("MOAN_FREQ: menu enabled={}"),enabled);
    }
    ImGuiMCP::SeparatorText(Tr("$TMS_DDF_MoanFreqTiers","Tier Intervals (seconds)").c_str());
    constexpr const char* names[]={"Low","Medium","High"};
    constexpr const wchar_t* boolKeys[]={L"LowEnabled",L"MediumEnabled",L"HighEnabled"};
    constexpr const wchar_t* sliderKeys[]={L"LowIntervalSeconds",L"MediumIntervalSeconds",L"HighIntervalSeconds"};
    for(int i=0;i<3;++i) {
        bool tierEnabled=sTierFrequencyEnabled[i].load();
        if(ImGuiMCP::Checkbox(fmt::format("Enable {}##ddf_moan_freq_enable_{}",names[i],i).c_str(),&tierEnabled)) {
            sTierFrequencyEnabled[i].store(tierEnabled);
            SaveFrequencyBool(boolKeys[i],tierEnabled);
        }
        float seconds=sTierIntervalSeconds[i].load();
        if(ImGuiMCP::SliderFloat(fmt::format("{} Interval##ddf_moan_freq_interval_{}",names[i],i).c_str(),
            &seconds,1.0f,15.0f,"%.2f s",ImGuiMCP::ImGuiSliderFlags_AlwaysClamp))
            sTierIntervalSeconds[i].store(std::clamp(seconds,1.0f,15.0f));
        if(ImGuiMCP::IsItemDeactivatedAfterEdit()) SaveFrequencyFloat(sliderKeys[i],sTierIntervalSeconds[i].load());
    }
    float gap=sSafetyGapSeconds.load();
    if(ImGuiMCP::SliderFloat(Tr("$TMS_DDF_MoanFreqGap","WAV Safety Gap (s)").c_str(),
        &gap,0.2f,2.0f,"%.2f s",ImGuiMCP::ImGuiSliderFlags_AlwaysClamp))
        sSafetyGapSeconds.store(std::clamp(gap,0.2f,2.0f));
    if(ImGuiMCP::IsItemDeactivatedAfterEdit()) SaveFrequencyFloat(L"SafetyGapSeconds",sSafetyGapSeconds.load());
    ImGuiMCP::TextDisabled("%s",Tr("$TMS_DDF_MoanFreqNote",
        "Effective cooldown = max(tier interval, current WAV duration + safety gap). Disabled tiers keep OStim native timing. No additional sound timer is created.").c_str());
    bool climaxOn=sClimaxEnabled.load();
    if(ImGuiMCP::Checkbox("Enable Matching Climax WAV##ddf_moan_climax",&climaxOn)) {
        sClimaxEnabled.store(climaxOn);
        const auto ini=GetPluginIniPathForLogging().wstring();
        WritePrivateProfileStringW(L"NativeMoanReplace",L"ClimaxEnabled",
            climaxOn?L"1":L"0",ini.c_str());
    }
    ImGuiMCP::TextDisabled("Climax needs Male/Climax or Female/Climax WAV files; no voice set/ESP. Actor-local VoiceGuard has priority.");
    bool voicePriority=sVoiceGuardPriority.load();
    if(ImGuiMCP::Checkbox("DDF Voice Priority (Experimental)##ddf_moan_voice",&voicePriority)) {
        sVoiceGuardPriority.store(voicePriority);
        SaveFrequencyBool(L"VoiceGuardPriority",voicePriority);
    }
    ImGuiMCP::TextDisabled("When DDF VoiceGuard is active for the same actor: call native OStim cooldown and suppress ordinary moan only if safely rearmed.");
    ImGuiMCP::TextDisabled("%s",IsReady()?"Native WAV replacement: ready":"Native WAV replacement: not active");
}

#endif

void __cdecl OnNativeBuildSound(RE::BSAudioManager* mgr,
    RE::BSSoundHandle* handle,void* descriptorData,std::uint32_t flags) noexcept {
    auto original=sOriginal.load(std::memory_order_acquire);
    if(!original) return;
    if(tContext.active && !tContext.consumed && tContext.file && mgr && handle) {
        tContext.consumed=true;
        bool built=false;
        try {built=BuildReplacement(mgr,*handle,tContext.file->path);} catch(...) {built=false;}
        if(built) {
            tContext.replaced=true;
            sReplaced.fetch_add(1,std::memory_order_relaxed);
            return;
        }
        try {logger::warn(MOAN_LOG("NATIVE_MOAN: reason=FileBuildRejected fallback=descriptor"));} catch(...) {}
    }
    original(mgr,handle,descriptorData,flags);
}
}
