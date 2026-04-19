#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hazuki {

struct RasterImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_alpha = false;
    std::vector<std::uint8_t> pixels;
};

bool IsCbgFile(const std::filesystem::path &path);
RasterImage DecodeCbg(const std::filesystem::path &path);
void EncodeCbg(const RasterImage &image, const std::filesystem::path &path);

}  // namespace hazuki
