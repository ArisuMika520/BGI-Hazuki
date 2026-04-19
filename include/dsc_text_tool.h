#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hazuki::dsc
{

    enum class ScriptContainerKind
    {
        DscCompressed,
        RawCompiled,
    };

    enum class TextKind
    {
        Name,
        Text,
        RubyKanji,
        RubyFurigana,
        Backlog,
        File,
        Other,
    };

    struct TextEntry
    {
        std::uint32_t index = 0;
        std::uint32_t text_offset = 0;
        TextKind kind = TextKind::Other;
        std::wstring comment;
        std::vector<std::uint32_t> code_offsets;
        std::vector<std::uint8_t> original_bytes;
        std::wstring original_text;
        std::wstring translation_text;
    };

    struct TextProject
    {
        ScriptContainerKind container_kind = ScriptContainerKind::DscCompressed;
        std::uint32_t decode_codepage = 932;
        std::uint32_t encode_codepage = 932;
        std::vector<TextEntry> entries;
    };

    bool IsDscScript(const std::filesystem::path &path);
    bool IsCompiledScript(const std::filesystem::path &path);

    std::filesystem::path GetDefaultProjectPath(const std::filesystem::path &script_path);
    std::filesystem::path InferScriptPathFromProject(const std::filesystem::path &project_path);
    std::filesystem::path GetDefaultPatchedPath(const std::filesystem::path &script_path);

    TextProject ExtractTextProject(
        const std::filesystem::path &script_path,
        std::uint32_t decode_codepage,
        std::uint32_t encode_codepage);

    void SaveTextProject(
        const TextProject &project,
        const std::filesystem::path &script_path,
        const std::filesystem::path &output_path);

    TextProject LoadTextProject(const std::filesystem::path &project_path);

    void ApplyTextProject(
        const std::filesystem::path &project_path,
        const std::filesystem::path &script_path,
        const std::filesystem::path &output_path,
        std::uint32_t fallback_encode_codepage);

    const wchar_t *ToString(TextKind kind);

} // namespace hazuki::dsc
