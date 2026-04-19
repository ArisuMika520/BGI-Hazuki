#include "asset_probe.h"

#include "bgi_cbg_codec.h"
#include "dsc_text_tool.h"
#include "png_io.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring mode = L"unpack";
    std::vector<std::filesystem::path> positional;
    std::uint32_t decode_codepage = 932;
    std::uint32_t encode_codepage = 932;
};

std::uint32_t ParseUInt(const std::wstring &value) {
    return static_cast<std::uint32_t>(std::stoul(value));
}

Options ParseOptions(int argc, wchar_t *argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"probe" || arg == L"unpack") {
            options.mode = arg;
            continue;
        }
        if (arg == L"--decode-cp" && i + 1 < argc) {
            options.decode_codepage = ParseUInt(argv[++i]);
            continue;
        }
        if (arg == L"--encode-cp" && i + 1 < argc) {
            options.encode_codepage = ParseUInt(argv[++i]);
            continue;
        }
        if (arg.rfind(L"--decode-cp=", 0) == 0) {
            options.decode_codepage = ParseUInt(arg.substr(12));
            continue;
        }
        if (arg.rfind(L"--encode-cp=", 0) == 0) {
            options.encode_codepage = ParseUInt(arg.substr(12));
            continue;
        }
        options.positional.emplace_back(arg);
    }
    return options;
}

void PrintUsage() {
    std::wcout << L"BGI_Hazuki AssetTool\n";
    std::wcout << L"用法:\n";
    std::wcout << L"  BGI_Hazuki_AssetTool probe <file-or-dir> [...]\n";
    std::wcout << L"  BGI_Hazuki_AssetTool unpack <file-or-dir> [...] [--decode-cp 932] [--encode-cp 932]\n";
    std::wcout << L"说明:\n";
    std::wcout << L"  自动识别 CBG 图像、DSC 脚本、BGI 音频(bw )，并分别导出 PNG / .hazuki.txt / OGG。\n";
}

void ProbeFile(const std::filesystem::path &path) {
    const auto info = hazuki::ProbeAsset(path);
    std::wcout << L"[PROBE] " << path << L" -> " << info.label;
    if (!info.suggested_extension.empty()) {
        std::wcout << L" (suggest " << info.suggested_extension << L")";
    }
    std::wcout << L'\n';
}

void UnpackFile(const std::filesystem::path &path, const Options &options, std::size_t &converted, std::size_t &skipped) {
    const auto info = hazuki::ProbeAsset(path);
    switch (info.kind) {
    case hazuki::AssetKind::CbgImage: {
        const auto output_path = path.wstring() + L".png";
        auto image = hazuki::DecodeCbg(path);
        hazuki::SavePng(image, output_path);
        std::wcout << L"[CBG->PNG] " << path << L" -> " << output_path << L'\n';
        ++converted;
        return;
    }
    case hazuki::AssetKind::DscScript:
    case hazuki::AssetKind::RawCompiledScript: {
        const auto project = hazuki::dsc::ExtractTextProject(path, options.decode_codepage, options.encode_codepage);
        const auto output_path = hazuki::dsc::GetDefaultProjectPath(path);
        hazuki::dsc::SaveTextProject(project, path, output_path);
        std::wcout << L"[SCRIPT->TXT] " << path << L" -> " << output_path << L'\n';
        ++converted;
        return;
    }
    case hazuki::AssetKind::BgiAudio: {
        const auto output_path = path.wstring() + L".ogg";
        hazuki::ExtractBgiAudioToOgg(path, output_path);
        std::wcout << L"[BW->OGG] " << path << L" -> " << output_path << L'\n';
        ++converted;
        return;
    }
    default:
        ++skipped;
        return;
    }
}

template <typename FileHandler>
void WalkInputs(const std::vector<std::filesystem::path> &paths, FileHandler &&handler) {
    for (const auto &path : paths) {
        if (std::filesystem::is_directory(path)) {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    handler(entry.path());
                }
            }
        } else {
            handler(path);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t *argv[]) {
    try {
        const auto options = ParseOptions(argc, argv);
        if (options.positional.empty()) {
            PrintUsage();
            return 1;
        }

        if (options.mode == L"probe") {
            WalkInputs(options.positional, [](const std::filesystem::path &path) {
                ProbeFile(path);
            });
            return 0;
        }

        hazuki::ImagingSession imaging;
        std::size_t converted = 0;
        std::size_t skipped = 0;
        WalkInputs(options.positional, [&](const std::filesystem::path &path) {
            UnpackFile(path, options, converted, skipped);
        });
        std::wcout << L"完成: 成功处理 " << converted << L" 个文件，跳过 " << skipped << L" 个文件。\n";
        return 0;
    } catch (const std::exception &ex) {
        std::wcerr << L"[FAILED] " << ex.what() << L'\n';
        return 1;
    }
}