#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// HAZUKI_CORE_API：用于在 BGI_Hazuki_Core.dll 与其调用方之间桥接符号导入/导出。
//   - 编译 DLL 本体时定义 HAZUKI_CORE_BUILD，将公共入口标记为 dllexport。
//   - GUI 等调用方编译时定义 HAZUKI_CORE_USE，将其标记为 dllimport。
//   - 其余独立 EXE 仍可静态链接同一份源码（两个宏都不定义 -> 退化为空）。
#if defined(HAZUKI_CORE_BUILD)
#define HAZUKI_CORE_API __declspec(dllexport)
#elif defined(HAZUKI_CORE_USE)
#define HAZUKI_CORE_API __declspec(dllimport)
#else
#define HAZUKI_CORE_API
#endif

namespace hazuki
{

    struct PipelineOptions
    {
        std::filesystem::path unpack_root;
        std::uint32_t decode_codepage = 932;
        std::uint32_t encode_codepage = 932;
    };

    struct PipelineProgress
    {
        std::size_t completed = 0;
        std::size_t total = 0;
        std::wstring current_item;
    };

    struct PipelineResult
    {
        std::filesystem::path unpack_root;
        std::size_t archive_count = 0;
        std::size_t extracted_count = 0;
        std::size_t processed_count = 0;
        std::size_t converted_count = 0;
        std::size_t skipped_count = 0;
    };

    struct PipelineCallbacks
    {
        std::function<void(const std::wstring &line)> on_log;
        std::function<void(const PipelineProgress &progress)> on_progress;
    };

    HAZUKI_CORE_API PipelineResult RunFullUnpackPipeline(
        const std::vector<std::filesystem::path> &inputs,
        const PipelineOptions &options,
        const PipelineCallbacks &callbacks = {});

} // namespace hazuki