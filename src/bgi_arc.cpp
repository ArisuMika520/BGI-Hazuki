#include "bgi_arc.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hazuki::arc
{
    namespace
    {

        constexpr std::array<std::uint8_t, 12> kArcMagic = {
            0x42,
            0x55,
            0x52,
            0x49,
            0x4B,
            0x4F,
            0x20,
            0x41,
            0x52,
            0x43,
            0x32,
            0x30,
        };
        constexpr std::size_t kHeaderSize = 16;
        constexpr std::size_t kEntrySize = 128;
        constexpr std::size_t kEntryNameSize = 96;

        std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("Failed to open file.");
            }
            file.seekg(0, std::ios::end);
            const auto size = static_cast<std::size_t>(file.tellg());
            file.seekg(0, std::ios::beg);

            std::vector<std::uint8_t> data(size);
            file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
            if (file.gcount() != static_cast<std::streamsize>(data.size()))
            {
                throw std::runtime_error("Failed to read file.");
            }
            return data;
        }

        std::wstring DecodeShiftJis(const std::uint8_t *bytes, std::size_t size)
        {
            const auto *char_bytes = reinterpret_cast<const char *>(bytes);
            std::size_t text_size = 0;
            while (text_size < size && char_bytes[text_size] != '\0')
            {
                ++text_size;
            }
            if (text_size == 0)
            {
                return L"";
            }

            const int wide_size = MultiByteToWideChar(932, 0, char_bytes, static_cast<int>(text_size), nullptr, 0);
            if (wide_size <= 0)
            {
                throw std::runtime_error("Failed to decode archive entry name.");
            }

            std::wstring output(static_cast<std::size_t>(wide_size), L'\0');
            MultiByteToWideChar(932, 0, char_bytes, static_cast<int>(text_size), output.data(), wide_size);
            return output;
        }

        std::filesystem::path SanitizeRelativePath(const std::wstring &value)
        {
            std::wstring normalized = value;
            std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            const std::filesystem::path input_path(normalized);
            if (input_path.has_root_name() || input_path.has_root_directory() || input_path.is_absolute())
            {
                throw std::runtime_error("Archive entry path must be relative.");
            }

            std::filesystem::path output;
            for (const auto &part : input_path)
            {
                const auto part_text = part.native();
                if (part_text.empty() || part_text == L".")
                {
                    continue;
                }
                if (part_text == L"..")
                {
                    throw std::runtime_error("Archive entry path cannot escape output directory.");
                }
                output /= part;
            }

            if (output.empty())
            {
                throw std::runtime_error("Archive entry path is empty.");
            }
            return output;
        }

        std::uint32_t ReadLe32(const std::uint8_t *bytes)
        {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }

    } // namespace

    bool IsBurikoArcFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::is_regular_file(path))
        {
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::array<std::uint8_t, kHeaderSize> header{};
        file.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
        if (file.gcount() != static_cast<std::streamsize>(header.size()))
        {
            return false;
        }

        return std::equal(kArcMagic.begin(), kArcMagic.end(), header.begin());
    }

    ArcArchiveInfo ReadArcArchiveInfo(const std::filesystem::path &path)
    {
        const auto bytes = ReadAllBytes(path);
        if (bytes.size() < kHeaderSize)
        {
            throw std::runtime_error("Archive header is truncated.");
        }
        if (!std::equal(kArcMagic.begin(), kArcMagic.end(), bytes.begin()))
        {
            throw std::runtime_error("Not a BURIKO ARC20 archive.");
        }

        const auto entry_count = ReadLe32(bytes.data() + 12);
        const std::size_t table_size = static_cast<std::size_t>(entry_count) * kEntrySize;
        const std::size_t base_offset = kHeaderSize + table_size;
        if (bytes.size() < base_offset)
        {
            throw std::runtime_error("Archive entry table is truncated.");
        }

        ArcArchiveInfo info;
        info.arc_path = path;
        info.entries.reserve(entry_count);

        for (std::uint32_t index = 0; index < entry_count; ++index)
        {
            const auto entry_offset = kHeaderSize + static_cast<std::size_t>(index) * kEntrySize;
            const auto *entry = bytes.data() + entry_offset;
            ArcEntry arc_entry;
            arc_entry.relative_path = SanitizeRelativePath(DecodeShiftJis(entry, kEntryNameSize));
            arc_entry.offset = ReadLe32(entry + kEntryNameSize);
            arc_entry.size = ReadLe32(entry + kEntryNameSize + 4);

            const std::size_t absolute_offset = base_offset + static_cast<std::size_t>(arc_entry.offset);
            if (absolute_offset + arc_entry.size > bytes.size())
            {
                throw std::runtime_error("Archive entry exceeds file size.");
            }
            info.entries.push_back(std::move(arc_entry));
        }

        return info;
    }

    std::vector<std::filesystem::path> ExtractArcArchive(
        const std::filesystem::path &arc_path,
        const std::filesystem::path &output_dir,
        const std::function<void(const ArcEntry &entry, const std::filesystem::path &output_path, std::size_t index, std::size_t total)> &on_entry)
    {
        const auto bytes = ReadAllBytes(arc_path);
        if (bytes.size() < kHeaderSize)
        {
            throw std::runtime_error("Archive header is truncated.");
        }
        if (!std::equal(kArcMagic.begin(), kArcMagic.end(), bytes.begin()))
        {
            throw std::runtime_error("Not a BURIKO ARC20 archive.");
        }

        const auto entry_count = ReadLe32(bytes.data() + 12);
        const std::size_t table_size = static_cast<std::size_t>(entry_count) * kEntrySize;
        const std::size_t base_offset = kHeaderSize + table_size;
        if (bytes.size() < base_offset)
        {
            throw std::runtime_error("Archive entry table is truncated.");
        }

        std::filesystem::create_directories(output_dir);
        std::vector<std::filesystem::path> extracted_paths;
        extracted_paths.reserve(entry_count);

        for (std::uint32_t index = 0; index < entry_count; ++index)
        {
            const auto entry_offset = kHeaderSize + static_cast<std::size_t>(index) * kEntrySize;
            const auto *entry = bytes.data() + entry_offset;

            ArcEntry arc_entry;
            arc_entry.relative_path = SanitizeRelativePath(DecodeShiftJis(entry, kEntryNameSize));
            arc_entry.offset = ReadLe32(entry + kEntryNameSize);
            arc_entry.size = ReadLe32(entry + kEntryNameSize + 4);

            const std::size_t absolute_offset = base_offset + static_cast<std::size_t>(arc_entry.offset);
            if (absolute_offset + arc_entry.size > bytes.size())
            {
                throw std::runtime_error("Archive entry exceeds file size.");
            }

            const auto output_path = output_dir / arc_entry.relative_path;
            std::filesystem::create_directories(output_path.parent_path());
            std::ofstream output(output_path, std::ios::binary);
            if (!output)
            {
                throw std::runtime_error("Failed to create extracted file.");
            }

            output.write(
                reinterpret_cast<const char *>(bytes.data() + absolute_offset),
                static_cast<std::streamsize>(arc_entry.size));
            if (!output)
            {
                throw std::runtime_error("Failed to write extracted file.");
            }

            extracted_paths.push_back(output_path);
            if (on_entry)
            {
                on_entry(arc_entry, output_path, static_cast<std::size_t>(index) + 1, static_cast<std::size_t>(entry_count));
            }
        }

        return extracted_paths;
    }

} // namespace hazuki::arc