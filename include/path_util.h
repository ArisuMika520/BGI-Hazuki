#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace hazuki
{

    /// 当路径长度接近或超过 MAX_PATH 时，为其添加 \\?\ 前缀以绕过 Win32 260 字符限制。
    /// 主要用于传递给 GDI+ 等不一定遵守 longPathAware 清单的旧式 Win32 API。
    inline std::filesystem::path ToExtendedPath(const std::filesystem::path &path)
    {
#ifdef _WIN32
        auto str = path.wstring();
        // 已经有前缀或者路径较短，直接返回
        if (str.size() < 248 || str.compare(0, 4, L"\\\\?\\") == 0)
        {
            return path;
        }
        // 确保为绝对路径
        auto abs = std::filesystem::absolute(path);
        auto abs_str = abs.wstring();
        // \\?\ 要求使用反斜杠
        std::replace(abs_str.begin(), abs_str.end(), L'/', L'\\');
        return std::filesystem::path(L"\\\\?\\" + abs_str);
#else
        return path;
#endif
    }

} // namespace hazuki
