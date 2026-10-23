#include "OStimMoanAutoLocator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TMS::DD::OStimMoanAutoLocator {
namespace {
    constexpr std::array<int, 53> kSignature = {0x8B, 0x83, -1, -1, -1, -1, 0x85, 0xC0, 0x7E, 0x2B, 0x83, 0xC0, 0xEC, 0x89, 0x83, -1, -1, -1, -1, 0x85, 0xC0, 0x7F, 0x1E, 0x4C, 0x8B, 0x83, -1, -1, -1, -1, 0x48, 0x8D, 0x93, -1, -1, -1, -1, 0x45, 0x33, 0xC9, 0x48, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, -1, -1, -1, -1};
    constexpr std::size_t kTailIndex = kSignature.size() - 5;
    constexpr std::uint32_t kExecutableFlag = 0x20000000;
    using Bytes = std::span<const std::uint8_t>;

    [[nodiscard]] bool Contains(std::size_t end, std::size_t at, std::size_t count) noexcept
    {
        return at <= end && count <= end - at;
    }
    [[nodiscard]] bool Read16(Bytes b, std::size_t at, std::uint16_t& value) noexcept
    {
        if (!Contains(b.size(), at, 2)) return false;
        value = static_cast<std::uint16_t>(b[at]) |
            (static_cast<std::uint16_t>(b[at + 1]) << 8);
        return true;
    }
    [[nodiscard]] bool Read32(Bytes b, std::size_t at, std::uint32_t& value) noexcept
    {
        if (!Contains(b.size(), at, 4)) return false;
        value = static_cast<std::uint32_t>(b[at]) |
            (static_cast<std::uint32_t>(b[at + 1]) << 8) |
            (static_cast<std::uint32_t>(b[at + 2]) << 16) |
            (static_cast<std::uint32_t>(b[at + 3]) << 24);
        return true;
    }
    [[nodiscard]] std::uint32_t U32(Bytes b, std::size_t at) noexcept
    {
        std::uint32_t n = 0;
        (void)Read32(b, at, n);
        return n;
    }
    [[nodiscard]] std::int32_t I32(Bytes b, std::size_t at) noexcept
    {
        const auto u = U32(b, at);

        std::int32_t result{};
        std::memcpy(&result, &u, sizeof(result));
        return result;
    }
    [[nodiscard]] bool Starts(Bytes b, std::initializer_list<std::uint8_t> bytes) noexcept
    {
        return b.size() >= bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), b.begin());
    }
    struct Section {
        std::uint32_t rva{}, rawSize{}, rawOffset{}, flags{};
    };
    struct Function {
        std::uint32_t start{}, end{};
    };
    class PEImage {
    public:
        explicit PEImage(Bytes bytes) : file_(bytes) {}
        [[nodiscard]] bool Parse(std::string& error)
        {
            if (file_.size() < 512 || !Starts(file_, {0x4d, 0x5a}))
                return Fail(error, "not a PE image");
            const auto pe = static_cast<std::size_t>(U32(file_, 0x3c));
            if (!Contains(file_.size(), pe, 24) ||
                !Starts(file_.subspan(pe), {0x50, 0x45, 0, 0}))
                return Fail(error, "invalid PE header");
            std::uint16_t machine{}, sectionCount{}, optionalSize{}, magic{};
            if (!Read16(file_, pe + 4, machine) ||
                !Read16(file_, pe + 6, sectionCount) ||
                !Read16(file_, pe + 20, optionalSize) ||
                machine != 0x8664 || sectionCount < 1 || sectionCount > 96)
                return Fail(error, "requires x64 PE");
            const auto optional = pe + 24;
            if (optionalSize < 112 || !Contains(file_.size(), optional, optionalSize) ||
                !Read16(file_, optional, magic) || magic != 0x20b)
                return Fail(error, "invalid PE32+ optional header");
            const auto sectionStart = optional + optionalSize;
            if (!Contains(file_.size(), sectionStart, static_cast<std::size_t>(sectionCount) * 40))
                return Fail(error, "truncated PE sections");
            imageSize_ = U32(file_, optional + 56);
            if (imageSize_ == 0 || imageSize_ > 0x80000000u)
                return Fail(error, "invalid image size");
            for (std::size_t i = 0; i < sectionCount; ++i) {
                const auto s = sectionStart + i * 40;
                Section sec{U32(file_, s + 12), U32(file_, s + 16),
                            U32(file_, s + 20), U32(file_, s + 36)};
                if (sec.rawSize && !Contains(file_.size(), sec.rawOffset, sec.rawSize))
                    return Fail(error, "truncated section");
                sections_.push_back(sec);
                if (sec.rawSize && (sec.flags & kExecutableFlag)) codeSections_.push_back(sec);
            }
            if (codeSections_.empty()) return Fail(error, "missing executable sections");


            if (U32(file_, optional + 108) < 4 || optionalSize < 112 + 4 * 8)
                return Fail(error, "missing exception directory");
            const auto pdataRva = U32(file_, optional + 112 + 3 * 8);
            const auto pdataSize = U32(file_, optional + 112 + 3 * 8 + 4);
            if (pdataSize < 12 || pdataSize % 12 || pdataSize > 16 * 1024 * 1024)
                return Fail(error, "invalid exception directory size");
            Bytes pdata;
            if (!Read(pdataRva, pdataSize, pdata))
                return Fail(error, "missing on-disk exception directory");
            for (std::size_t i = 0; i < pdata.size(); i += 12) {
                const auto start = U32(pdata, i);
                const auto end = U32(pdata, i + 4);
                if (0 < start && start < end && end <= imageSize_)
                    functions_.push_back({start, end});
            }
            std::sort(functions_.begin(), functions_.end(),
                [](const Function& a, const Function& b) { return a.start < b.start; });
            if (functions_.empty()) return Fail(error, "no valid unwind function entries");
            return true;
        }
        [[nodiscard]] bool Read(std::uint32_t rva, std::size_t n, Bytes& out) const noexcept
        {
            for (const auto& s : sections_) {
                if (rva >= s.rva && n <= s.rawSize &&
                    static_cast<std::uint64_t>(rva) + n <=
                        static_cast<std::uint64_t>(s.rva) + s.rawSize) {
                    const auto off = static_cast<std::size_t>(s.rawOffset) + (rva - s.rva);
                    if (Contains(file_.size(), off, n)) {
                        out = file_.subspan(off, n);
                        return true;
                    }
                }
            }
            return false;
        }
        [[nodiscard]] bool IsCode(std::uint32_t rva, std::size_t n = 1) const noexcept
        {
            for (const auto& s : codeSections_) {
                if (rva >= s.rva && n <= s.rawSize &&
                    static_cast<std::uint64_t>(rva) + n <=
                        static_cast<std::uint64_t>(s.rva) + s.rawSize) return true;
            }
            return false;
        }
        [[nodiscard]] const Function* FunctionContaining(std::uint32_t rva) const noexcept
        {
            const auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                [](std::uint32_t value, const Function& f) { return value < f.start; });
            if (it == functions_.begin()) return nullptr;
            const auto& f = *(it - 1);
            return rva < f.end ? &f : nullptr;
        }
        [[nodiscard]] const std::vector<Section>& CodeSections() const noexcept { return codeSections_; }
    private:
        static bool Fail(std::string& error, const char* text) { error = text; return false; }
        Bytes file_{};
        std::uint32_t imageSize_{};
        std::vector<Section> sections_;
        std::vector<Section> codeSections_;
        std::vector<Function> functions_;
    };
    [[nodiscard]] bool Matches(Bytes b, std::size_t offset) noexcept
    {
        if (!Contains(b.size(), offset, kSignature.size())) return false;
        for (std::size_t j = 0; j < kSignature.size(); ++j) {
            if (kSignature[j] >= 0 && b[offset + j] != kSignature[j]) return false;
        }
        return true;
    }
    [[nodiscard]] bool HasTimerWrite(Bytes begin, std::uint32_t offset) noexcept
    {
        for (std::size_t i = 0; i + 6 <= begin.size() && i + 6 <= 42; ++i) {
            if (begin[i] == 0x89 && begin[i+1] == 0x81 && U32(begin, i+2) == offset)
                return true;
        }
        return false;
    }
    [[nodiscard]] bool CalculateTarget(std::uint32_t afterJump, std::int32_t delta,
        std::uint32_t& output) noexcept
    {
        const auto value = static_cast<std::int64_t>(afterJump) + delta;
        if (value < 0 || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
        output = static_cast<std::uint32_t>(value);
        return true;
    }
    [[nodiscard]] bool Validate(const PEImage& image, std::uint32_t start,
        ScanResult& result, std::string& reason)
    {
        auto reject = [&reason](const char* text) { reason = text; return false; };
        Bytes code;
        if (!image.Read(start, kSignature.size(), code) || !Matches(code, 0))
            return reject("moan countdown signature mismatch");
        const auto timer1 = U32(code, 2);
        const auto timer2 = U32(code, 15);
        const auto partner = U32(code, 26);
        const auto moan = U32(code, 33);
        if (timer1 != timer2) return reject("timer read/write offsets differ");
        if (timer1 % 4 || timer1 < 0x80 || timer1 > 0x4000)
            return reject("implausible cooldown offset");
        if (!(0x40 <= partner && partner < moan && moan < timer1 &&
              partner % 8 == 0 && moan % 8 == 0))
            return reject("implausible partner/moan offsets");
        const auto tail = static_cast<std::uint32_t>(start + kTailIndex);
        std::uint32_t play{};
        if (!CalculateTarget(tail + 5, I32(code, kTailIndex + 1), play) ||
            !image.IsCode(play)) return reject("invalid playSound tail-jump target");
        const auto* loopFunction = image.FunctionContaining(start);
        const auto* playFunction = image.FunctionContaining(play);
        if (!loopFunction) return reject("moan block outside unwind function");
        if (!playFunction || playFunction->start != play)
            return reject("playSound is not unwind function entry");
        if (loopFunction->start >= start || loopFunction->end <
                static_cast<std::uint64_t>(start) + kSignature.size() ||
            loopFunction->end - loopFunction->start >= 0x2000)
            return reject("loopSound invalid function boundary");
        Bytes begin;
        if (!image.Read(loopFunction->start,
            std::min<std::uint32_t>(96, loopFunction->end - loopFunction->start), begin) ||
            !Starts(begin, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81}))
            return reject("loopSound prologue mismatch");
        if (begin.size() < 27 || U32(begin, 8) != timer1-4 ||
            !HasTimerWrite(begin, timer1-4))
            return reject("loopSound preceding timer mismatch");
        std::vector<std::uint32_t> actorCandidates;
        for (std::size_t i = 0; i + 7 <= begin.size(); ++i)
            if (begin[i] == 0x48 && begin[i+1] == 0x81 && begin[i+2] == 0xC1)
                actorCandidates.push_back(U32(begin, i+3));
        const auto playLen = playFunction->end - playFunction->start;
        if (playLen < 0x100 || playLen > 0x2000)
            return reject("implausible playSound length");
        Bytes playBegin;
        if (!image.Read(play, std::min<std::uint32_t>(96, playLen), playBegin) ||
            !Starts(playBegin, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41}))
            return reject("playSound prologue mismatch");
        bool timerReset = false;
        for (std::size_t i = 0; i + 10 <= playBegin.size(); ++i)
            if (playBegin[i] == 0xC7 && playBegin[i+1] == 0x81 &&
                U32(playBegin, i+2) == timer1 && U32(playBegin, i+6) == 0)
                timerReset = true;
        if (!timerReset) return reject("playSound does not reset same cooldown");
        Bytes playBody;
        if (!image.Read(play, playLen, playBody)) return reject("playSound body truncated");

        std::vector<std::pair<std::uint32_t, std::uint32_t>> actorCounts;
        for (std::size_t i = 0; i + 7 <= playBody.size(); ++i) {
            if (playBody[i] != 0x48 || playBody[i+1] != 0x8B || playBody[i+2] != 0x97)
                continue;
            const auto offset = U32(playBody, i+3);
            if (offset < 0x20 || offset > 0x2000 || offset % 8) continue;
            auto it = std::find_if(actorCounts.begin(), actorCounts.end(),
                [offset](const auto& pair) { return pair.first == offset; });
            if (it == actorCounts.end()) actorCounts.emplace_back(offset, 1);
            else ++it->second;
        }
        std::uint32_t actorOffset = 0, frequent = 0;
        for (const auto& [offset, n] : actorCounts) {
            if (n >= 2) { ++frequent; actorOffset = offset; }
        }
        if (frequent != 1) return reject("actor pointer offset is not uniquely recoverable");
        if (std::find(actorCandidates.begin(), actorCandidates.end(), actorOffset) == actorCandidates.end())
            return reject("actor field disagrees between loopSound and playSound");
        Bytes final;
        if (!image.Read(playFunction->end-5, 5, final) || final[0] != 0xE9)
            return reject("playSound exit is not a tail-jump");
        std::uint32_t cooldown{};
        if (!CalculateTarget(playFunction->end, I32(final, 1), cooldown) ||
            !image.IsCode(cooldown, 13)) return reject("cooldown target is not executable");
        const auto* coolFunction = image.FunctionContaining(cooldown);
        if (!coolFunction || coolFunction->start != cooldown)
            return reject("cooldown target is not function entry");
        Bytes coolBegin;
        if (!image.Read(cooldown, 13, coolBegin) ||
            !Starts(coolBegin, {0x40,0x57,0x48,0x83,0xEC}) ||
            coolBegin[6] != 0x83 || coolBegin[7] != 0xB9 ||
            U32(coolBegin, 8) != timer1)
            return reject("cooldown function/timer signature mismatch");





        if (play <= cooldown || play - cooldown > 0x400 ||
            coolFunction->end > play)
            return reject("cooldown/playSound layout not safely bounded");
        const auto coolLen = play - cooldown;
        Bytes coolBody;
        if (!image.Read(cooldown, coolLen, coolBody))
            return reject("cooldown body truncated");
        bool nativeTimerWrite = false;
        for (std::size_t i = 0; i + 6 <= coolBody.size(); ++i) {
            if (coolBody[i] == 0x89 && coolBody[i+1] == 0x87 &&
                U32(coolBody, i+2) == timer1)
                nativeTimerWrite = true;
        }
        if (!nativeTimerWrite) return reject("cooldown does not write same timer");

        const auto finalRet = std::find(coolBody.rbegin(), coolBody.rend(), std::uint8_t{0xC3});
        if (finalRet == coolBody.rend()) return reject("cooldown has no return");
        const auto suffix = std::distance(coolBody.rbegin(), finalRet);
        if (suffix > 16 || !std::all_of(coolBody.end() - suffix, coolBody.end(),
            [](std::uint8_t c) { return c == 0xCC || c == 0x90; }))
            return reject("cooldown epilogue not bounded");
        result.loopEndRva = loopFunction->end;
        result.playEndRva = playFunction->end;
        result.cooldownEndRva = play;
        result.loopRva = loopFunction->start;
        result.hookRva = tail;
        result.playRva = play;
        result.cooldownRva = cooldown;
        result.actorOffset = actorOffset;
        result.partnerOffset = partner;
        result.moanOffset = moan;
        result.cooldownOffset = timer1;
        return true;
    }
}

ScanResult Scan(std::span<const std::uint8_t> file)
{
    ScanResult result;
    try {
        PEImage image(file);
        if (!image.Parse(result.reason)) return result;
        std::string firstError;
        for (const auto& section : image.CodeSections()) {
            Bytes code;
            if (!image.Read(section.rva, section.rawSize, code)) {
                result.reason = "executable section is not file-backed";
                return result;
            }
            for (std::size_t i = 0; i + kSignature.size() <= code.size(); ++i) {
                if (code[i] != 0x8B || code[i+1] != 0x83 || !Matches(code, i)) continue;
                ++result.candidateCount;
                ScanResult candidate;
                std::string error;
                if (Validate(image, section.rva + static_cast<std::uint32_t>(i), candidate, error)) {
                    ++result.validatedCount;
                    if (result.validatedCount == 1) {
                        result.loopEndRva = candidate.loopEndRva;
                        result.playEndRva = candidate.playEndRva;
                        result.cooldownEndRva = candidate.cooldownEndRva;
                        result.loopRva = candidate.loopRva;
                        result.hookRva = candidate.hookRva;
                        result.playRva = candidate.playRva;
                        result.cooldownRva = candidate.cooldownRva;
                        result.actorOffset = candidate.actorOffset;
                        result.partnerOffset = candidate.partnerOffset;
                        result.moanOffset = candidate.moanOffset;
                        result.cooldownOffset = candidate.cooldownOffset;
                    }
                } else if (firstError.empty()) {
                    firstError = std::move(error);
                }
            }
        }
        if (result.candidateCount == 1 && result.validatedCount == 1) {
            result.validated = true;
            result.reason = "STRUCTURE_VALIDATED";
        } else if (result.candidateCount == 0) {
            result.reason = "NoMoanPattern";
        } else if (result.candidateCount > 1) {
            result.reason = "AmbiguousMoanPattern";
        } else {
            result.reason = firstError.empty() ? "StructureMismatch" : firstError;
        }
    } catch (const std::exception& e) {
        result.reason = std::string("ScanException: ") + e.what();
    } catch (...) {
        result.reason = "UnknownScanException";
    }
    return result;
}

SoundBuildCallResult ScanSoundBuildCall(std::span<const std::uint8_t> file)
{
    SoundBuildCallResult result;
    try {
        PEImage image(file);
        if (!image.Parse(result.reason)) return result;



        constexpr std::array<int, 41> pattern{
            0x48,0x8B,0xD7,0xE8,-1,-1,-1,-1,
            0x48,0x8B,0xCB,0xE8,-1,-1,-1,-1,
            0x48,0x8B,0xD0,0x48,0x8B,0xCF,0xE8,-1,-1,-1,-1,
            0x0F,0x28,0xCE,0x48,0x8B,0xCF,0xE8,-1,-1,-1,-1,
            0x48,0x8B,0xCF
        };
        for (const auto& section : image.CodeSections()) {
            Bytes code;
            if (!image.Read(section.rva, section.rawSize, code)) {
                result.reason = "truncated code section";return result;
            }
            for (std::size_t i=0;i+pattern.size()<=code.size();++i) {
                if (code[i]!=0x48 || code[i+1]!=0x8B || code[i+2]!=0xD7)continue;
                bool matches=true;
                for (std::size_t j=0;j<pattern.size();++j) {
                    if (pattern[j]>=0 && code[i+j]!=pattern[j]) {matches=false;break;}
                }
                if (!matches)continue;
                ++result.candidateCount;
                const auto chain=section.rva+static_cast<std::uint32_t>(i);
                const auto call=chain+3;
                const auto* fn=image.FunctionContaining(call);
                if (!fn || fn->start>=call ||
                    fn->end < chain+pattern.size() ||
                    fn->end-fn->start<0x65 || fn->end-fn->start>0x120)continue;
                Bytes prologue;
                if (!image.Read(fn->start, 16, prologue) ||
                    !Starts(prologue,{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,
                    0x83,0xEC,0x30,0x0F,0x29,0x74,0x24,0x20}))continue;



                Bytes preceding;
                if (chain<fn->start+38 || !image.Read(chain-38,38,preceding))continue;
                bool descriptor=false;
                for (std::size_t k=0;k+9<=preceding.size();++k) {
                    if (Starts(preceding.subspan(k),
                        {0x48,0x8B,0x0F,0x33,0xD2,0x48,0x83,0xC7,0x08})) {
                        descriptor=true;break;
                    }
                }
                if (!descriptor)continue;

                bool descriptorFlags=false;
                for (std::size_t k=0;k+4<=preceding.size();++k) {
                    if (Starts(preceding.subspan(k),{0x41,0xB9,0x10,0x00,0x00,0x00}) ||
                        Starts(preceding.subspan(k),{0x44,0x8D,0x4A,0x10})){
                        descriptorFlags=true;break;
                    }
                }
                if (!descriptorFlags)continue;
                std::uint32_t target{};
                if (!CalculateTarget(call+5,I32(code,i+4),target) ||
                    !image.IsCode(target))continue;
                ++result.validCount;
                if (result.validCount==1) {
                    result.gameSoundPlayRva=fn->start;
                    result.gameSoundPlayEndRva=fn->end;
                    result.buildCallRva=call;
                    result.originalTargetRva=target;
                }
            }
        }
        if(result.candidateCount==1 && result.validCount==1){
            result.validated=true;result.reason="STRUCTURE_VALIDATED";
        }else if(result.candidateCount==0){result.reason="NoSoundBuildCallPattern";}
        else if(result.candidateCount!=1){result.reason="AmbiguousSoundBuildCallPattern";}
        else result.reason="SoundBuildCallStructureRejected";
    }catch(const std::exception& e){result.reason=std::string("ScanException: ")+e.what();}
    catch(...){result.reason="UnknownScanException";}
    return result;
}


ClimaxCallResult ScanClimaxCall(std::span<const std::uint8_t> file, const ScanResult& ordinary)
{
    ClimaxCallResult result;
    if (!ordinary.validated) { result.reason = "OrdinaryScanUnvalidated"; return result; }
    try {
        PEImage image(file);
        if (!image.Parse(result.reason)) return result;













        for (const auto& section : image.CodeSections()) {
            Bytes bytes;
            if (!image.Read(section.rva, section.rawSize, bytes)) {
                result.reason = "UnbackedCodeSection"; return result;
            }
            for (std::size_t i = 0; i + 35 <= bytes.size(); ++i) {
                const auto b = bytes.subspan(i);
                if (b[0] != 0x4c || b[1] != 0x8b) continue;



                const auto reg = static_cast<std::uint8_t>(b[2] & 7u);
                if ((reg != 3 && reg != 7) || b[2] != (0x80u | reg)) continue;
                if (!Starts(b.subspan(7), {0x48,0x8d}) ||
                    b[9] != (0x90u | reg) ||
                    !Starts(b.subspan(14), {0x41,0xb1,0x01,0xc7}) ||
                    b[18] != (0x80u | reg) ||
                    !Starts(b.subspan(23), {0x00,0x00,0x00,0x00,0x48,0x8b}) ||
                    b[29] != (0xC8u | reg) || b[30] != 0xe8) continue;
                ++result.candidateCount;
                const auto at = section.rva + static_cast<std::uint32_t>(i);
                const auto call = at + 30;
                const auto partner = U32(b,3);
                const auto climax = U32(b,10);
                const auto countdown = U32(b,19);
                if (partner != ordinary.partnerOffset ||
                    climax != ordinary.moanOffset + 0x48u ||
                    countdown != ordinary.cooldownOffset + 0x28u) continue;
                std::uint32_t target{};
                if (!CalculateTarget(call+5,I32(b,31),target) ||
                    target != ordinary.playRva) continue;
                const auto* fn = image.FunctionContaining(call);
                if (!fn || fn->start >= at || fn->end < call+8 ||
                    fn->end-fn->start < 0x70 || fn->end-fn->start > 0x300) continue;
                Bytes prologue;
                const auto size=std::min<std::uint32_t>(16u,fn->end-fn->start);
                if (!image.Read(fn->start,size,prologue)) continue;
                const bool rdiPrologue = reg==7 && Starts(prologue,
                    {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20});
                const bool rbxPrologue = reg==3 && Starts(prologue,
                    {0x40,0x53,0x48,0x83,0xec,0x20});
                if (!rdiPrologue && !rbxPrologue) continue;




                Bytes after;
                const auto remaining = fn->end - (call+5);
                if (!image.Read(call+5,std::min<std::uint32_t>(remaining,40u),after)) continue;
                bool postCallReaction=false;
                for (std::size_t k=0;k+7<=after.size();++k) {
                    if (after[k]==0x48 && after[k+1]==0x8d &&
                        after[k+2]==(0x88u|reg) &&
                        U32(after,k+3)==ordinary.moanOffset+0x90u) {
                        postCallReaction=true; break;
                    }
                }
                if (!postCallReaction) continue;
                ++result.validCount;
                if (result.validCount==1) {
                    result.callRva=call;
                    result.originalTargetRva=target;
                    result.functionRva=fn->start;
                    result.functionEndRva=fn->end;
                }
            }
        }
        if(result.candidateCount==1 && result.validCount==1) {
            result.validated=true; result.reason="STRUCTURE_VALIDATED";
        } else if(result.candidateCount==0) result.reason="NoClimaxCallPattern";
        else if(result.candidateCount!=1) result.reason="AmbiguousClimaxCallPattern";
        else result.reason="ClimaxCallStructureRejected";
    } catch(const std::exception& e) { result.reason=std::string("ScanException: ")+e.what(); }
    catch(...) {result.reason="UnknownScanException";}
    return result;
}

}
