#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hazuki::arc {

struct ArcEntry {
    std::filesystem::path relative_path;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct ArcArchiveInfo {
    std::filesystem::path arc_path;
    std::vector<ArcEntry> entries;
};

bool IsBurikoArcFile(const std::filesystem::path &path);
ArcArchiveInfo ReadArcArchiveInfo(const std::filesystem::path &path);

std::vector<std::filesystem::path> ExtractArcArchive(
    const std::filesystem::path &arc_path,
    const std::filesystem::path &output_dir,
    const std::function<void(const ArcEntry &entry, const std::filesystem::path &output_path, std::size_t index, std::size_t total)> &on_entry = {});

}  // namespace hazuki::arc