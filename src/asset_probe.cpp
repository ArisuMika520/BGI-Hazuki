#include "asset_probe.h"

#include "bgi_cbg_codec.h"
#include "dsc_text_tool.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace hazuki
{
    namespace
    {

        std::vector<std::uint8_t> ReadHeader(const std::filesystem::path &path, std::size_t size)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("Failed to open file.");
            }

            std::vector<std::uint8_t> data(size);
            input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
            data.resize(static_cast<std::size_t>(input.gcount()));
            return data;
        }

        bool StartsWith(const std::vector<std::uint8_t> &data, const char *magic, std::size_t length)
        {
            if (data.size() < length)
            {
                return false;
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                if (data[i] != static_cast<std::uint8_t>(magic[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool HasBytesAt(const std::vector<std::uint8_t> &data, std::size_t offset, const char *magic, std::size_t length)
        {
            if (data.size() < offset + length)
            {
                return false;
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                if (data[offset + i] != static_cast<std::uint8_t>(magic[i]))
                {
                    return false;
                }
            }
            return true;
        }

        AssetInfo MakeInfo(AssetKind kind, const wchar_t *label, const wchar_t *extension)
        {
            AssetInfo info;
            info.kind = kind;
            info.label = label;
            info.suggested_extension = extension;
            return info;
        }

    } // namespace

    AssetInfo ProbeAsset(const std::filesystem::path &path)
    {
        if (!std::filesystem::is_regular_file(path))
        {
            return MakeInfo(AssetKind::Unknown, L"not-a-file", L"");
        }

        if (IsCbgFile(path))
        {
            return MakeInfo(AssetKind::CbgImage, L"cbg-image", L".png");
        }
        if (dsc::IsDscScript(path))
        {
            return MakeInfo(AssetKind::DscScript, L"dsc-script", L".hazuki.txt");
        }
        if (dsc::IsCompiledScript(path))
        {
            return MakeInfo(AssetKind::RawCompiledScript, L"compiled-script", L".hazuki.txt");
        }
        if (IsBgiAudioFile(path))
        {
            return MakeInfo(AssetKind::BgiAudio, L"bgi-audio", L".ogg");
        }

        const auto header = ReadHeader(path, 16);
        if (StartsWith(header, "\x89PNG\r\n\x1A\n", 8))
        {
            return MakeInfo(AssetKind::PngImage, L"png-image", L".png");
        }
        if (StartsWith(header, "\xFF\xD8\xFF", 3))
        {
            return MakeInfo(AssetKind::JpegImage, L"jpeg-image", L".jpg");
        }
        if (StartsWith(header, "BM", 2))
        {
            return MakeInfo(AssetKind::BmpImage, L"bmp-image", L".bmp");
        }
        if (StartsWith(header, "OggS", 4))
        {
            return MakeInfo(AssetKind::OggAudio, L"ogg-audio", L".ogg");
        }
        if (StartsWith(header, "RIFF", 4) && HasBytesAt(header, 8, "WAVE", 4))
        {
            return MakeInfo(AssetKind::WavAudio, L"wav-audio", L".wav");
        }

        return MakeInfo(AssetKind::Unknown, L"unknown", L"");
    }

    bool IsBgiAudioFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::is_regular_file(path))
        {
            return false;
        }
        const auto header = ReadHeader(path, 8);
        return HasBytesAt(header, 4, "bw  ", 4);
    }

    void ExtractBgiAudioToOgg(const std::filesystem::path &input_path, const std::filesystem::path &output_path)
    {
        std::ifstream input(input_path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Failed to open input audio file.");
        }

        std::uint32_t header_size = 0;
        input.read(reinterpret_cast<char *>(&header_size), sizeof(header_size));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(header_size)))
        {
            throw std::runtime_error("Audio header is truncated.");
        }

        const auto header = ReadHeader(input_path, 12);
        if (!HasBytesAt(header, 4, "bw  ", 4))
        {
            throw std::runtime_error("Not a recognized BGI audio file.");
        }

        std::uint32_t payload_size = 0;
        input.seekg(8, std::ios::beg);
        input.read(reinterpret_cast<char *>(&payload_size), sizeof(payload_size));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(payload_size)))
        {
            throw std::runtime_error("Audio header is truncated.");
        }

        input.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
        std::vector<char> payload(payload_size);
        input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size()))
        {
            throw std::runtime_error("Audio payload is truncated.");
        }

        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path, std::ios::binary);
        if (!output)
        {
            throw std::runtime_error("Failed to create output audio file.");
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    const wchar_t *ToString(AssetKind kind)
    {
        switch (kind)
        {
        case AssetKind::CbgImage:
            return L"cbg-image";
        case AssetKind::DscScript:
            return L"dsc-script";
        case AssetKind::RawCompiledScript:
            return L"compiled-script";
        case AssetKind::BgiAudio:
            return L"bgi-audio";
        case AssetKind::PngImage:
            return L"png-image";
        case AssetKind::JpegImage:
            return L"jpeg-image";
        case AssetKind::BmpImage:
            return L"bmp-image";
        case AssetKind::OggAudio:
            return L"ogg-audio";
        case AssetKind::WavAudio:
            return L"wav-audio";
        default:
            return L"unknown";
        }
    }

} // namespace hazuki