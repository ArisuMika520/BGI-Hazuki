#include "dsc_text_tool.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring mode;
    std::vector<std::filesystem::path> positional;
    std::uint32_t decode_codepage = 932;
    std::uint32_t encode_codepage = 0;
};

bool EndsWithInsensitive(const std::wstring &value, const std::wstring &suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(), [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
}

std::uint32_t ParseUInt(const std::wstring &value) {
    return static_cast<std::uint32_t>(std::stoul(value));
}

void PrintUsage() {
    std::wcout << L"BGI_Hazuki TextTool\n";
    std::wcout << L"用法:\n";
    std::wcout << L"  BGI_Hazuki_TextTool extract <script> [output.hazuki.txt] [--decode-cp 932] [--encode-cp 932]\n";
    std::wcout << L"  BGI_Hazuki_TextTool apply <script.hazuki.txt> [output_script] [--encode-cp 932]\n";
    std::wcout << L"  也可以直接拖入一个 DSC 脚本进行提取，或拖入一个 .hazuki.txt 项目进行回写。\n";
}

Options ParseOptions(int argc, wchar_t *argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"extract" || arg == L"apply") {
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

void ExtractOne(const Options &options, const std::filesystem::path &script_path, const std::optional<std::filesystem::path> &output_path) {
    const auto encode_cp = options.encode_codepage == 0 ? options.decode_codepage : options.encode_codepage;
    const auto project = hazuki::dsc::ExtractTextProject(script_path, options.decode_codepage, encode_cp);
    const auto destination = output_path.value_or(hazuki::dsc::GetDefaultProjectPath(script_path));
    hazuki::dsc::SaveTextProject(project, script_path, destination);
    std::wcout << L"[EXTRACT] " << script_path << L" -> " << destination << L"\n";
}

void ApplyOne(const Options &options, const std::filesystem::path &project_path, const std::optional<std::filesystem::path> &output_path) {
    const auto script_path = hazuki::dsc::InferScriptPathFromProject(project_path);
    const auto destination = output_path.value_or(hazuki::dsc::GetDefaultPatchedPath(script_path));
    hazuki::dsc::ApplyTextProject(project_path, script_path, destination, options.encode_codepage);
    std::wcout << L"[APPLY] " << project_path << L" -> " << destination << L"\n";
}

void RunAuto(const Options &options) {
    if (options.positional.empty()) {
        PrintUsage();
        throw std::runtime_error("Missing input file.");
    }

    for (const auto &path : options.positional) {
        const auto filename = path.filename().wstring();
        if (EndsWithInsensitive(filename, L".hazuki.txt")) {
            ApplyOne(options, path, std::nullopt);
        } else {
            ExtractOne(options, path, std::nullopt);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t *argv[]) {
    try {
        const auto options = ParseOptions(argc, argv);
        if (options.mode.empty()) {
            RunAuto(options);
            return 0;
        }
        if (options.mode == L"extract") {
            if (options.positional.empty() || options.positional.size() > 2) {
                PrintUsage();
                return 1;
            }
            ExtractOne(options, options.positional[0], options.positional.size() == 2 ? std::optional<std::filesystem::path>(options.positional[1]) : std::nullopt);
            return 0;
        }
        if (options.mode == L"apply") {
            if (options.positional.empty() || options.positional.size() > 2) {
                PrintUsage();
                return 1;
            }
            ApplyOne(options, options.positional[0], options.positional.size() == 2 ? std::optional<std::filesystem::path>(options.positional[1]) : std::nullopt);
            return 0;
        }
        PrintUsage();
        return 1;
    } catch (const std::exception &ex) {
        std::wcerr << L"[FAILED] " << ex.what() << L"\n";
        return 1;
    }
}
