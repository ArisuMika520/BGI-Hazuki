#include "bgi_cbg_codec.h"
#include "png_io.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void ProcessFile(const std::filesystem::path &path, std::size_t &converted, std::size_t &skipped) {
    try {
        if (!std::filesystem::is_regular_file(path)) {
            ++skipped;
            return;
        }

        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);

        if (extension == L".png") {
            const auto output_path = path.parent_path() / (path.stem().wstring() + L".cbg");
            auto image = hazuki::LoadPng(path);
            hazuki::EncodeCbg(image, output_path);
            std::wcout << L"[PNG->CBG] " << path << L" -> " << output_path << L'\n';
            ++converted;
            return;
        }

        if (hazuki::IsCbgFile(path)) {
            const auto output_path = path.wstring() + L".png";
            auto image = hazuki::DecodeCbg(path);
            hazuki::SavePng(image, output_path);
            std::wcout << L"[CBG->PNG] " << path << L" -> " << output_path << L'\n';
            ++converted;
            return;
        }

        ++skipped;
    } catch (const std::exception &ex) {
        std::wcerr << L"[FAILED] " << path << L" : " << ex.what() << L'\n';
    }
}

void ProcessPath(const std::filesystem::path &path, std::size_t &converted, std::size_t &skipped) {
    if (std::filesystem::is_directory(path)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                ProcessFile(entry.path(), converted, skipped);
            }
        }
        return;
    }

    ProcessFile(path, converted, skipped);
}

}  // namespace

int wmain(int argc, wchar_t *argv[]) {
    if (argc < 2) {
        std::wcout << L"BGI_Hazuki ImageTool\n";
        std::wcout << L"用法: 拖入一个或多个 PNG / CompressedBG 文件，或直接拖入目录。\n";
        return 1;
    }

    try {
        hazuki::ImagingSession imaging;
        std::size_t converted = 0;
        std::size_t skipped = 0;

        for (int i = 1; i < argc; ++i) {
            ProcessPath(argv[i], converted, skipped);
        }

        std::wcout << L"完成: 成功处理 " << converted << L" 个文件，跳过 " << skipped << L" 个文件。\n";
        return 0;
    } catch (const std::exception &ex) {
        std::wcerr << L"初始化失败: " << ex.what() << L'\n';
        return 1;
    }
}