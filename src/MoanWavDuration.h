#pragma once
#include <filesystem>
namespace TMS::DD::MoanWavDuration {
float ReadDuration(const std::filesystem::path& path) noexcept;
}
