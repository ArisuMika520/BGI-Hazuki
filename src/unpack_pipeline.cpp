#include "unpack_pipeline.h"

#include "asset_probe.h"
#include "bgi_arc.h"
#include "bgi_cbg_codec.h"
#include "dsc_text_tool.h"
#include "png_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <stdexcept>
#include <vector>

namespace hazuki
{
    namespace
    {

        void EmitLog(const PipelineCallbacks &callbacks, const std::wstring &line)
        {
            if (callbacks.on_log)
            {
                callbacks.on_log(line);
            }
        }

        void EmitProgress(const PipelineCallbacks &callbacks, std::size_t completed, std::size_t total, const std::wstring &current_item)
        {
            if (callbacks.on_progress)
            {
                PipelineProgress progress;
                progress.completed = completed;
                progress.total = total;
                progress.current_item = current_item;
                callbacks.on_progress(progress);
            }
        }

        bool EndsWithInsensitive(const std::wstring &value, const std::wstring &suffix)
        {
            if (suffix.size() > value.size())
            {
                return false;
            }
            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(), [](wchar_t left, wchar_t right)
                              { return std::towlower(left) == std::towlower(right); });
        }

        std::vector<std::filesystem::path> CollectArcFiles(const std::vector<std::filesystem::path> &inputs)
        {
            std::vector<std::filesystem::path> output;
            for (const auto &input : inputs)
            {
                if (std::filesystem::is_directory(input))
                {
                    for (const auto &entry : std::filesystem::recursive_directory_iterator(input))
                    {
                        if (!entry.is_regular_file())
                        {
                            continue;
                        }
                        if (EndsWithInsensitive(entry.path().extension().wstring(), L".arc") && arc::IsBurikoArcFile(entry.path()))
                        {
                            output.push_back(entry.path());
                        }
                    }
                    continue;
                }

                if (std::filesystem::is_regular_file(input) && EndsWithInsensitive(input.extension().wstring(), L".arc") && arc::IsBurikoArcFile(input))
                {
                    output.push_back(input);
                }
            }

            std::sort(output.begin(), output.end());
            output.erase(std::unique(output.begin(), output.end()), output.end());
            return output;
        }

        std::filesystem::path MakeUniqueOutputDir(
            const std::filesystem::path &root,
            const std::filesystem::path &arc_path,
            std::set<std::wstring> &used_names)
        {
            std::wstring base_name = arc_path.stem().wstring();
            if (base_name.empty())
            {
                base_name = L"archive";
            }
            std::wstring candidate = base_name;
            std::size_t suffix = 2;
            while (!used_names.insert(candidate).second)
            {
                candidate = base_name + L"_" + std::to_wstring(suffix++);
            }
            return root / candidate;
        }

        void ProcessExtractedFile(
            const std::filesystem::path &path,
            const PipelineOptions &options,
            PipelineResult &result,
            const PipelineCallbacks &callbacks,
            std::size_t &progress_value,
            std::size_t total_work)
        {
            const auto info = ProbeAsset(path);
            const auto current_item = std::wstring(L"处理 ") + path.filename().wstring();

            try
            {
                switch (info.kind)
                {
                case AssetKind::CbgImage:
                {
                    const auto output_path = path.wstring() + L".png";
                    EmitLog(callbacks, std::wstring(L"[CBG] 解析 ") + path.wstring() +
                                           L" (文件大小: " + std::to_wstring(std::filesystem::file_size(path)) + L" 字节)");
                    auto image = DecodeCbg(path);
                    SavePng(image, output_path);
                    EmitLog(callbacks, std::wstring(L"[CBG->PNG] ") + path.wstring() + L" -> " + output_path);
                    ++result.converted_count;
                    // PNG 可通过 EncodeCbg 反向生成 CBG，原始无扩展名文件不再需要保留。
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    if (!ec)
                    {
                        EmitLog(callbacks, std::wstring(L"[CLEAN] ") + path.wstring());
                    }
                    break;
                }
                case AssetKind::DscScript:
                case AssetKind::RawCompiledScript:
                {
                    const auto project = dsc::ExtractTextProject(path, options.decode_codepage, options.encode_codepage);
                    const auto output_path = dsc::GetDefaultProjectPath(path);
                    dsc::SaveTextProject(project, path, output_path);
                    EmitLog(callbacks, std::wstring(L"[SCRIPT->TXT] ") + path.wstring() + L" -> " + output_path.wstring());
                    ++result.converted_count;
                    break;
                }
                case AssetKind::BgiAudio:
                {
                    const auto output_path = path.wstring() + L".ogg";
                    ExtractBgiAudioToOgg(path, output_path);
                    EmitLog(callbacks, std::wstring(L"[BW->OGG] ") + path.wstring() + L" -> " + output_path);
                    ++result.converted_count;
                    break;
                }
                default:
                    ++result.skipped_count;
                    EmitLog(callbacks, std::wstring(L"[SKIP] ") + path.wstring() + L" -> unknown/unsupported");
                    break;
                }
            }
            catch (const std::exception &ex)
            {
                ++result.skipped_count;
                const auto narrow = std::string(ex.what());
                const int wide_len = MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), static_cast<int>(narrow.size()), nullptr, 0);
                std::wstring reason(static_cast<std::size_t>(wide_len > 0 ? wide_len : 0), L'\0');
                if (wide_len > 0)
                {
                    MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), static_cast<int>(narrow.size()), reason.data(), wide_len);
                }
                else
                {
                    reason = std::wstring(narrow.begin(), narrow.end());
                }
                EmitLog(callbacks, std::wstring(L"[ERROR] ") + path.wstring() + L" -> 转换失败: " + reason);
                if (path.wstring().size() >= 240)
                {
                    EmitLog(callbacks, std::wstring(L"[HINT] 该路径长度为 ") + std::to_wstring(path.wstring().size()) + L" 字符，接近或超过 Windows MAX_PATH (260) 限制，建议将输出目录设置为更短的路径。");
                }
            }

            ++result.processed_count;
            ++progress_value;
            EmitProgress(callbacks, progress_value, total_work, current_item);
        }

    } // namespace

    PipelineResult RunFullUnpackPipeline(
        const std::vector<std::filesystem::path> &inputs,
        const PipelineOptions &options,
        const PipelineCallbacks &callbacks)
    {
        const auto arc_files = CollectArcFiles(inputs);
        if (arc_files.empty())
        {
            throw std::runtime_error("No valid .arc files were provided.");
        }

        PipelineResult result;
        result.unpack_root = options.unpack_root.empty() ? (std::filesystem::current_path() / L"unpack") : options.unpack_root;
        std::filesystem::create_directories(result.unpack_root);

        EmitLog(callbacks, std::wstring(L"[ROOT] 输出目录: ") + result.unpack_root.wstring());
        EmitLog(callbacks, std::wstring(L"[SCAN] 发现可处理 ARC 文件: ") + std::to_wstring(arc_files.size()));

        std::vector<arc::ArcArchiveInfo> archives;
        archives.reserve(arc_files.size());
        std::size_t total_entries = 0;
        for (const auto &arc_path : arc_files)
        {
            auto info = arc::ReadArcArchiveInfo(arc_path);
            total_entries += info.entries.size();
            EmitLog(callbacks, std::wstring(L"[ARC] ") + arc_path.wstring() + L" : " + std::to_wstring(info.entries.size()) + L" 个文件");
            archives.push_back(std::move(info));
        }

        const std::size_t total_work = total_entries * 2;
        std::size_t progress_value = 0;
        EmitProgress(callbacks, progress_value, total_work, L"等待开始");

        ImagingSession imaging;
        std::set<std::wstring> used_output_names;
        result.archive_count = archives.size();

        for (const auto &archive_info : archives)
        {
            const auto output_dir = MakeUniqueOutputDir(result.unpack_root, archive_info.arc_path, used_output_names);
            if (std::filesystem::exists(output_dir))
            {
                std::filesystem::remove_all(output_dir);
            }
            std::filesystem::create_directories(output_dir);
            EmitLog(callbacks, std::wstring(L"[UNPACK] ") + archive_info.arc_path.wstring() + L" -> " + output_dir.wstring());

            const auto extracted_paths = arc::ExtractArcArchive(
                archive_info.arc_path,
                output_dir,
                [&](const arc::ArcEntry &entry, const std::filesystem::path &, std::size_t, std::size_t)
                {
                    ++result.extracted_count;
                    ++progress_value;
                    EmitProgress(
                        callbacks,
                        progress_value,
                        total_work,
                        std::wstring(L"解包 ") + entry.relative_path.wstring());
                });

            for (const auto &extracted_path : extracted_paths)
            {
                ProcessExtractedFile(extracted_path, options, result, callbacks, progress_value, total_work);
            }
        }

        EmitLog(
            callbacks,
            std::wstring(L"[DONE] ARC=") + std::to_wstring(result.archive_count) + L", extracted=" + std::to_wstring(result.extracted_count) + L", processed=" + std::to_wstring(result.processed_count) + L", converted=" + std::to_wstring(result.converted_count) + L", skipped=" + std::to_wstring(result.skipped_count));
        EmitProgress(callbacks, total_work, total_work, L"完成");

        return result;
    }

} // namespace hazuki