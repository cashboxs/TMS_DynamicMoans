#include "PCH.h"
#include "MoanWavDuration.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
namespace TMS::DD::MoanWavDuration {
namespace {
std::uint32_t U32(const unsigned char* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) |
           (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
}
std::uint16_t U16(const unsigned char* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1])<<8);
}
}
float ReadDuration(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream in(path,std::ios::binary | std::ios::ate);
        if(!in) return 0;
        const auto size=static_cast<std::streamoff>(in.tellg());
        if(size<44 || size>static_cast<std::streamoff>(64*1024*1024)) return 0;
        in.seekg(0);
        std::array<unsigned char,12> riff{};
        if(!in.read(reinterpret_cast<char*>(riff.data()),riff.size()) ||
           std::memcmp(riff.data(),"RIFF",4) ||
           std::memcmp(riff.data()+8,"WAVE",4)) return 0;
        std::uint32_t rate=0;
        std::uint64_t dataBytes=0;
        bool validFmt=false;
        std::uint64_t offset=12;
        const auto fileLen=static_cast<std::uint64_t>(size);
        while(offset+8<=fileLen) {
            std::array<unsigned char,8> ch{};
            in.seekg(static_cast<std::streamoff>(offset));
            if(!in.read(reinterpret_cast<char*>(ch.data()),ch.size())) return 0;
            const auto bytes=U32(ch.data()+4);
            if(bytes>fileLen-offset-8) return 0;
            if(!std::memcmp(ch.data(),"fmt ",4) && bytes>=16) {
                std::array<unsigned char,16> fmt{};
                if(!in.read(reinterpret_cast<char*>(fmt.data()),fmt.size())) return 0;
                const auto codec=U16(fmt.data());
                rate=U32(fmt.data()+8);
                validFmt=(codec==1 || codec==3 || codec==0xFFFE) && rate>=1000 && rate<10'000'000;
            } else if(!std::memcmp(ch.data(),"data",4)) {
                dataBytes += bytes;
            }
            offset += 8+bytes+(bytes&1u);
        }
        if(!validFmt || !dataBytes) return 0;
        const double seconds=static_cast<double>(dataBytes)/static_cast<double>(rate);
        return (seconds>0 && seconds<=300) ? static_cast<float>(seconds) : 0;
    } catch(...) { return 0; }
}
}
