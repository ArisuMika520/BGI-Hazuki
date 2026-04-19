#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hazuki {

enum class AssetKind {
    Unknown,
    CbgImage,
    DscScript,
    RawCompiledScript,
    BgiAudio,
    PngImage,
    JpegImage,
    BmpImage,
    OggAudio,
    WavAudio,
};

struct AssetInfo {
    AssetKind kind = AssetKind::Unknown;
    std::wstring label;
    std::wstring suggested_extension;
};

AssetInfo ProbeAsset(const std::filesystem::path &path);
bool IsBgiAudioFile(const std::filesystem::path &path);
void ExtractBgiAudioToOgg(const std::filesystem::path &input_path, const std::filesystem::path &output_path);
const wchar_t *ToString(AssetKind kind);

}  // namespace hazuki