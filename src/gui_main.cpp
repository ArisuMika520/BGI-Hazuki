#include "unpack_pipeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <WindowsX.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include <Shellapi.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

    constexpr wchar_t kWindowClassName[] = L"BGI_HAZUKI_GUI_WINDOW_V4";
    constexpr UINT kMessageLog = WM_APP + 1;
    constexpr UINT kMessageProgress = WM_APP + 2;
    constexpr UINT kMessageFinished = WM_APP + 3;

    enum class HitTarget
    {
        none,
        drop_zone,
        choose_button,
        output_button,
    };

    struct ProgressPayload
    {
        std::size_t completed = 0;
        std::size_t total = 0;
        std::wstring current_item;
    };

    struct FinishedPayload
    {
        bool success = false;
        bool auto_close = false;
        std::wstring message;
        std::filesystem::path output_dir;
    };

    struct AutoRunConfig
    {
        bool auto_close = false;
        std::vector<std::filesystem::path> inputs;
    };

    struct Layout
    {
        Gdiplus::RectF drop_zone;
        Gdiplus::RectF choose_button;
        Gdiplus::RectF output_button;
        Gdiplus::RectF progress_track;
        Gdiplus::RectF log_view;
    };

    struct AppState
    {
        HWND window = nullptr;
        std::unique_ptr<Gdiplus::Bitmap> background_bitmap;
        std::unique_ptr<Gdiplus::Bitmap> icon_bitmap;
        std::unique_ptr<Gdiplus::PrivateFontCollection> private_fonts;
        std::wstring font_face = L"Microsoft YaHei UI";
        HICON icon_handle = nullptr;
        ULONG_PTR gdiplus_token = 0;
        bool processing = false;
        bool auto_close = false;
        bool has_result = false;
        bool last_run_success = false;
        bool mouse_tracking = false;
        HitTarget hot_target = HitTarget::none;
        HitTarget pressed_target = HitTarget::none;
        int client_width = 0;
        int client_height = 0;
        int log_offset = 0;
        std::size_t completed = 0;
        std::size_t total = 0;
        std::wstring current_item = L"等待拖放 .arc 文件";
        std::wstring status_message = L"就绪";
        std::wstring note = L"直接拖入单个或多个 .arc 文件，解析期间将自动提取结构目录，并分流文字与图像。\n或者点击下方按钮手动选择。";
        std::filesystem::path last_output_dir;
        std::vector<std::wstring> logs;
        std::thread worker;
    };

    float Clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    Gdiplus::RectF OffsetRect(const Gdiplus::RectF &rect, float dx, float dy)
    {
        return Gdiplus::RectF(rect.X + dx, rect.Y + dy, rect.Width, rect.Height);
    }

    bool ContainsPoint(const Gdiplus::RectF &rect, POINT point)
    {
        return point.x >= rect.X && point.x <= rect.GetRight() && point.y >= rect.Y && point.y <= rect.GetBottom();
    }

    void AddRoundedRect(Gdiplus::GraphicsPath &path,
                        const Gdiplus::RectF &rect,
                        float radius)
    {
        if (radius <= 0.5f)
        {
            path.AddRectangle(rect);
            return;
        }
        float diameter = (std::min)(radius * 2.0f, (std::min)(rect.Width, rect.Height));
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
        path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    }

    std::filesystem::path ExecutableDirectory()
    {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};
        return std::filesystem::path(buffer.data()).parent_path();
    }

    std::filesystem::path FindAsset(const std::vector<std::filesystem::path> &candidates)
    {
        const std::array<std::filesystem::path, 2> seeds = {ExecutableDirectory(), std::filesystem::current_path()};
        for (const auto &seed : seeds)
        {
            auto base = seed;
            for (int depth = 0; depth < 5 && !base.empty(); ++depth)
            {
                for (const auto &relative : candidates)
                {
                    std::error_code ec;
                    const auto file = (base / relative).lexically_normal();
                    if (std::filesystem::exists(file, ec) && !ec)
                        return file;
                }
                if (!base.has_parent_path())
                    break;
                const auto parent = base.parent_path();
                if (parent == base)
                    break;
                base = parent;
            }
        }
        return {};
    }

    std::wstring FirstPrivateFontFamilyName(const Gdiplus::PrivateFontCollection &collection)
    {
        const int count = collection.GetFamilyCount();
        if (count <= 0)
            return {};
        auto families = std::make_unique<Gdiplus::FontFamily[]>(count);
        INT found = 0;
        collection.GetFamilies(count, families.get(), &found);
        if (found <= 0)
            return {};
        std::array<wchar_t, LF_FACESIZE> name{};
        families[0].GetFamilyName(name.data());
        return name.data();
    }

    void LoadAssets(AppState *state)
    {
        const auto bg = FindAsset({L"static/bg.png", L"BGI_Hazuki/static/bg.png"});
        if (!bg.empty())
        {
            auto bitmap = std::make_unique<Gdiplus::Bitmap>(bg.c_str());
            if (bitmap->GetLastStatus() == Gdiplus::Ok)
                state->background_bitmap = std::move(bitmap);
        }

        const auto icon = FindAsset({L"static/icon.png", L"BGI_Hazuki/static/icon.png"});
        if (!icon.empty())
        {
            auto bitmap = std::make_unique<Gdiplus::Bitmap>(icon.c_str());
            if (bitmap->GetLastStatus() == Gdiplus::Ok)
                state->icon_bitmap = std::move(bitmap);
        }

        const std::vector<std::filesystem::path> fonts = {
            L"附赠/推荐字体包/NotoSansSC-Medium.ttf",
            L"附赠/推荐字体包/SourceHanSansCN-Medium.otf",
            L"../附赠/推荐字体包/NotoSansSC-Medium.ttf",
            L"../附赠/推荐字体包/SourceHanSansCN-Medium.otf",
        };
        for (const auto &candidate : fonts)
        {
            const auto file = FindAsset({candidate});
            if (file.empty())
                continue;
            auto collection = std::make_unique<Gdiplus::PrivateFontCollection>();
            if (collection->AddFontFile(file.c_str()) != Gdiplus::Ok)
                continue;
            const auto family = FirstPrivateFontFamilyName(*collection);
            if (!family.empty())
            {
                state->font_face = family;
                state->private_fonts = std::move(collection);
                break;
            }
        }
    }

    std::unique_ptr<Gdiplus::Font> MakeFont(const AppState *state, float size, INT style)
    {
        const auto *collection = state->private_fonts ? state->private_fonts.get() : nullptr;
        auto font = std::make_unique<Gdiplus::Font>(state->font_face.c_str(), size, style, Gdiplus::UnitPixel, collection);
        if (font->GetLastStatus() == Gdiplus::Ok)
            return font;
        return std::make_unique<Gdiplus::Font>(L"Microsoft YaHei UI", size, style, Gdiplus::UnitPixel);
    }

    void DrawText(Gdiplus::Graphics &g, const AppState *state, const std::wstring &text,
                  const Gdiplus::RectF &rect, float size, INT style, const Gdiplus::Color &color,
                  Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear,
                  Gdiplus::StringAlignment line_align = Gdiplus::StringAlignmentNear,
                  bool wrap = true)
    {
        auto font = MakeFont(state, size, style);
        Gdiplus::StringFormat format;
        format.SetAlignment(align);
        format.SetLineAlignment(line_align);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        if (!wrap)
            format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush brush(color);
        g.DrawString(text.c_str(), -1, font.get(), rect, &format, &brush);
    }

    Layout ComputeLayout(const AppState *state)
    {
        Layout layout;
        const float margin = 48.0f;
        const float content_width = (std::max)(420.0f, static_cast<float>(state->client_width) - margin * 2.0f);
        const float content_height = static_cast<float>(state->client_height);
        const float header_h = 110.0f;

        layout.drop_zone = Gdiplus::RectF(margin, header_h + 24.0f, content_width, 200.0f);

        const float button_h = 38.0f;
        const float button_w = 132.0f;
        const float gap = 12.0f;
        const float buttons_y = layout.drop_zone.GetBottom() + 24.0f;
        layout.choose_button = Gdiplus::RectF(margin, buttons_y, button_w, button_h);
        layout.output_button = Gdiplus::RectF(margin + button_w + gap, buttons_y, button_w, button_h);

        layout.progress_track = Gdiplus::RectF(margin, buttons_y + button_h + 28.0f, content_width, 3.0f);

        const float log_y = layout.progress_track.GetBottom() + 36.0f;
        layout.log_view = Gdiplus::RectF(margin, log_y, content_width, (std::max)(120.0f, content_height - log_y - margin));
        return layout;
    }

    int VisibleLogLines(const AppState *state)
    {
        const Layout layout = ComputeLayout(state);
        return (std::max)(1, static_cast<int>(layout.log_view.Height / 22.0f));
    }

    int MaxLogOffset(const AppState *state)
    {
        return (std::max)(0, static_cast<int>(state->logs.size()) - VisibleLogLines(state));
    }

    std::wstring Timestamped(const std::wstring &line)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buffer[32];
        swprintf_s(buffer, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buffer) + L"  " + line;
    }

    void AppendLog(AppState *state, const std::wstring &line)
    {
        const bool follow_tail = state->log_offset >= MaxLogOffset(state) - 1;
        if (state->logs.size() >= 500)
        {
            state->logs.erase(state->logs.begin(), state->logs.begin() + 100);
            state->log_offset = (std::max)(0, state->log_offset - 100);
        }
        state->logs.push_back(Timestamped(line));
        state->log_offset = follow_tail ? MaxLogOffset(state) : std::clamp(state->log_offset, 0, MaxLogOffset(state));
        InvalidateRect(state->window, nullptr, FALSE);
    }

    HitTarget HitTest(const AppState *state, POINT point)
    {
        const Layout layout = ComputeLayout(state);
        if (!state->processing && ContainsPoint(layout.choose_button, point))
            return HitTarget::choose_button;
        if (!state->last_output_dir.empty() && ContainsPoint(layout.output_button, point))
            return HitTarget::output_button;
        return HitTarget::none;
    }

    void DrawImageWithOpacity(Gdiplus::Graphics &g, Gdiplus::Image *image,
                              const Gdiplus::RectF &rect, float opacity)
    {
        if (!image || image->GetLastStatus() != Gdiplus::Ok)
            return;
        Gdiplus::ImageAttributes attrs;
        Gdiplus::ColorMatrix matrix{};
        matrix.m[0][0] = 1.0f;
        matrix.m[1][1] = 1.0f;
        matrix.m[2][2] = 1.0f;
        matrix.m[4][4] = 1.0f;
        matrix.m[3][3] = Clamp01(opacity);
        attrs.SetColorMatrix(&matrix);
        g.DrawImage(image, rect, 0.0f, 0.0f, static_cast<Gdiplus::REAL>(image->GetWidth()), static_cast<Gdiplus::REAL>(image->GetHeight()), Gdiplus::UnitPixel, &attrs);
    }

    void DrawUiFlat(Gdiplus::Graphics &g, const AppState *state, const Layout &layout)
    {
        const float w = static_cast<float>(state->client_width);
        const float h = static_cast<float>(state->client_height);

        const Gdiplus::Color kAccent(255, 182, 96, 114);
        const Gdiplus::Color kInk(255, 38, 38, 42);
        const Gdiplus::Color kInkSoft(255, 110, 110, 116);
        const Gdiplus::Color kInkMuted(255, 168, 168, 172);
        const Gdiplus::Color kHair(255, 230, 230, 232);

        // 1. Base canvas: paper white
        Gdiplus::SolidBrush base(Gdiplus::Color(255, 252, 251, 251));
        g.FillRectangle(&base, 0.0f, 0.0f, w, h);

        // 2. Background photo (cover-fit) at full opacity
        if (state->background_bitmap && state->background_bitmap->GetLastStatus() == Gdiplus::Ok)
        {
            const float iw = static_cast<float>(state->background_bitmap->GetWidth());
            const float ih = static_cast<float>(state->background_bitmap->GetHeight());
            const float scale = (std::max)(w / iw, h / ih);
            const float dw = iw * scale, dh = ih * scale;
            DrawImageWithOpacity(g, state->background_bitmap.get(),
                                 Gdiplus::RectF((w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh), 1.0f);
        }

        // 3. Soft white veil — keep BG visible but text clearly readable
        Gdiplus::SolidBrush veil(Gdiplus::Color(165, 252, 251, 251));
        g.FillRectangle(&veil, 0.0f, 0.0f, w, h);

        const float left = layout.drop_zone.X;
        const float right = layout.drop_zone.GetRight();

        // 4. Header — icon · title · status
        const float icon_size = 40.0f;
        const float icon_y = 44.0f;
        if (state->icon_bitmap && state->icon_bitmap->GetLastStatus() == Gdiplus::Ok)
        {
            Gdiplus::GraphicsPath clip;
            AddRoundedRect(clip, Gdiplus::RectF(left, icon_y, icon_size, icon_size), 10.0f);
            auto saved = g.Save();
            g.SetClip(&clip);
            DrawImageWithOpacity(g, state->icon_bitmap.get(), Gdiplus::RectF(left, icon_y, icon_size, icon_size), 1.0f);
            g.Restore(saved);
        }

        const float title_x = left + icon_size + 14.0f;
        DrawText(g, state, L"BGI Hazuki", Gdiplus::RectF(title_x, icon_y - 2.0f, 360.0f, 26.0f),
                 20.0f, Gdiplus::FontStyleRegular, kInk);
        DrawText(g, state, L"BGI / Ethornell 引擎资源解包套件",
                 Gdiplus::RectF(title_x, icon_y + 24.0f, 420.0f, 18.0f),
                 12.0f, Gdiplus::FontStyleRegular, kInkSoft);

        // status pill (top-right)
        {
            const std::wstring text = state->processing ? state->current_item : state->status_message;
            Gdiplus::Color dot_color = state->processing ? kAccent : Gdiplus::Color(255, 110, 180, 130);
            if (state->has_result && !state->last_run_success)
                dot_color = Gdiplus::Color(255, 230, 90, 90);
            const float pill_h = 24.0f;
            auto font = MakeFont(state, 12.0f, Gdiplus::FontStyleRegular);
            Gdiplus::RectF measure(0.0f, 0.0f, 400.0f, pill_h);
            Gdiplus::RectF bounds;
            Gdiplus::StringFormat fmt;
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            g.MeasureString(text.c_str(), -1, font.get(), measure, &fmt, &bounds);
            const float pill_w = bounds.Width + 36.0f;
            const float pill_x = right - pill_w;
            const float pill_y = icon_y + (icon_size - pill_h) * 0.5f;
            Gdiplus::SolidBrush pill_bg(Gdiplus::Color(180, 248, 248, 248));
            Gdiplus::GraphicsPath pill_path;
            AddRoundedRect(pill_path, Gdiplus::RectF(pill_x, pill_y, pill_w, pill_h), pill_h * 0.5f);
            g.FillPath(&pill_bg, &pill_path);
            Gdiplus::Pen pill_pen(kHair, 1.0f);
            g.DrawPath(&pill_pen, &pill_path);
            Gdiplus::SolidBrush dot_brush(dot_color);
            g.FillEllipse(&dot_brush, pill_x + 12.0f, pill_y + pill_h * 0.5f - 4.0f, 8.0f, 8.0f);
            DrawText(g, state, text, Gdiplus::RectF(pill_x + 24.0f, pill_y, pill_w - 32.0f, pill_h),
                     12.0f, Gdiplus::FontStyleRegular, kInkSoft, Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter, false);
        }

        // hairline divider under header
        Gdiplus::Pen hair(kHair, 1.0f);
        g.DrawLine(&hair, left, icon_y + icon_size + 22.0f, right, icon_y + icon_size + 22.0f);

        // 5. Drop zone — minimal dashed frame, centered prompt
        Gdiplus::Pen drop_pen(Gdiplus::Color(200, 182, 96, 114), 1.2f);
        Gdiplus::REAL dash[] = {6.0f, 5.0f};
        drop_pen.SetDashPattern(dash, 2);
        Gdiplus::GraphicsPath drop_path;
        AddRoundedRect(drop_path, layout.drop_zone, 14.0f);
        Gdiplus::SolidBrush drop_fill(Gdiplus::Color(110, 255, 255, 255));
        g.FillPath(&drop_fill, &drop_path);
        g.DrawPath(&drop_pen, &drop_path);

        const std::wstring big_text = state->processing ? state->current_item : L"拖入 .arc 文件到此处";
        DrawText(g, state, big_text,
                 Gdiplus::RectF(layout.drop_zone.X, layout.drop_zone.Y + layout.drop_zone.Height * 0.5f - 36.0f,
                                layout.drop_zone.Width, 32.0f),
                 20.0f, Gdiplus::FontStyleRegular, kInk,
                 Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter, false);
        DrawText(g, state, state->processing ? L"正在解析与分流，请稍候" : L"支持多文件批量处理 · 自动识别图像 / 文本 / 音频",
                 Gdiplus::RectF(layout.drop_zone.X, layout.drop_zone.Y + layout.drop_zone.Height * 0.5f + 4.0f,
                                layout.drop_zone.Width, 22.0f),
                 13.0f, Gdiplus::FontStyleRegular, kInkSoft,
                 Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter, false);

        // 6. Buttons — primary filled · secondary text-ghost
        auto DrawPrimary = [&](const Gdiplus::RectF &rect, const std::wstring &text, bool hover, bool pressed, bool enabled)
        {
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, rect, rect.Height * 0.5f);
            BYTE alpha = enabled ? (pressed ? 230 : (hover ? 255 : 245)) : 90;
            Gdiplus::SolidBrush fill(Gdiplus::Color(alpha, kAccent.GetR(), kAccent.GetG(), kAccent.GetB()));
            g.FillPath(&fill, &path);
            DrawText(g, state, text, rect, 13.0f, Gdiplus::FontStyleRegular,
                     Gdiplus::Color(255, 255, 255, 255),
                     Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter, false);
        };
        auto DrawGhost = [&](const Gdiplus::RectF &rect, const std::wstring &text, bool hover, bool pressed, bool enabled)
        {
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, rect, rect.Height * 0.5f);
            Gdiplus::SolidBrush fill(hover && enabled ? Gdiplus::Color(60, 182, 96, 114) : Gdiplus::Color(0, 0, 0, 0));
            g.FillPath(&fill, &path);
            Gdiplus::Pen pen(enabled ? Gdiplus::Color(180, 182, 96, 114) : kHair, 1.0f);
            g.DrawPath(&pen, &path);
            Gdiplus::Color tc = enabled ? (pressed ? Gdiplus::Color(255, 140, 70, 88) : kAccent) : kInkMuted;
            DrawText(g, state, text, rect, 13.0f, Gdiplus::FontStyleRegular, tc,
                     Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter, false);
        };

        DrawPrimary(layout.choose_button, L"选择文件",
                    state->hot_target == HitTarget::choose_button,
                    state->pressed_target == HitTarget::choose_button,
                    !state->processing);
        DrawGhost(layout.output_button, L"打开输出目录",
                  state->hot_target == HitTarget::output_button,
                  state->pressed_target == HitTarget::output_button,
                  !state->last_output_dir.empty());

        // 7. Progress — slim hairline
        {
            std::wstring p_text;
            if (state->total > 0)
                p_text = std::to_wstring(state->completed) + L" / " + std::to_wstring(state->total);
            else
                p_text = L"—";
            DrawText(g, state, L"进度",
                     Gdiplus::RectF(layout.progress_track.X, layout.progress_track.Y - 22.0f, 200.0f, 18.0f),
                     11.0f, Gdiplus::FontStyleRegular, kInkMuted);
            DrawText(g, state, p_text,
                     Gdiplus::RectF(layout.progress_track.X, layout.progress_track.Y - 22.0f, layout.progress_track.Width, 18.0f),
                     11.0f, Gdiplus::FontStyleRegular, kInkSoft, Gdiplus::StringAlignmentFar, Gdiplus::StringAlignmentNear, false);

            Gdiplus::SolidBrush track_bg(kHair);
            g.FillRectangle(&track_bg, layout.progress_track.X, layout.progress_track.Y, layout.progress_track.Width, layout.progress_track.Height);
            const float p = state->total == 0 ? 0.0f : Clamp01(static_cast<float>(state->completed) / static_cast<float>(state->total));
            if (p > 0.0f)
            {
                Gdiplus::SolidBrush fill(kAccent);
                g.FillRectangle(&fill, layout.progress_track.X, layout.progress_track.Y, layout.progress_track.Width * p, layout.progress_track.Height);
            }
        }

        // 8. Log section
        DrawText(g, state, L"日志",
                 Gdiplus::RectF(layout.log_view.X, layout.log_view.Y - 28.0f, 200.0f, 18.0f),
                 11.0f, Gdiplus::FontStyleRegular, kInkMuted);

        if (state->logs.empty())
        {
            DrawText(g, state, L"暂无记录", layout.log_view,
                     13.0f, Gdiplus::FontStyleRegular, kInkMuted,
                     Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentNear, false);
            return;
        }

        const auto saved = g.Save();
        g.SetClip(layout.log_view, Gdiplus::CombineModeReplace);
        const float line_height = 22.0f;
        const int visible = VisibleLogLines(state);
        const int start = std::clamp(state->log_offset, 0, MaxLogOffset(state));
        float y = layout.log_view.Y;

        for (int index = 0; index < visible; ++index)
        {
            int li = start + index;
            if (li >= static_cast<int>(state->logs.size()))
                break;
            const auto &line = state->logs[li];
            Gdiplus::Color tc = kInkSoft;
            if (line.find(L"[FAILED]") != std::wstring::npos)
                tc = Gdiplus::Color(255, 220, 80, 80);
            else if (line.find(L"[DONE]") != std::wstring::npos)
                tc = Gdiplus::Color(255, 90, 160, 110);
            else if (line.find(L"[START]") != std::wstring::npos)
                tc = kAccent;

            // 8 chars timestamp, then 2 spaces, then content (per Timestamped())
            std::wstring ts = line.substr(0, (std::min<size_t>)(8u, line.size()));
            std::wstring rest = line.size() > 10 ? line.substr(10) : L"";
            DrawText(g, state, ts,
                     Gdiplus::RectF(layout.log_view.X, y, 64.0f, line_height),
                     11.0f, Gdiplus::FontStyleRegular, kInkMuted,
                     Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter, false);
            DrawText(g, state, rest,
                     Gdiplus::RectF(layout.log_view.X + 70.0f, y, layout.log_view.Width - 70.0f, line_height),
                     12.0f, Gdiplus::FontStyleRegular, tc,
                     Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter, false);
            y += line_height;
        }
        g.Restore(saved);
    }

    void PaintUi(AppState *state, HDC hdc)
    {
        RECT rc{};
        GetClientRect(state->window, &rc);
        int width = rc.right - rc.left, height = rc.bottom - rc.top;
        if (width <= 0 || height <= 0)
            return;
        HDC memory_dc = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP old_bitmap = static_cast<HBITMAP>(SelectObject(memory_dc, bitmap));
        {
            Gdiplus::Graphics g(memory_dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            DrawUiFlat(g, state, ComputeLayout(state));
        }
        BitBlt(hdc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
    }

    void PostLog(HWND window, std::wstring line)
    {
        auto payload = std::make_unique<std::wstring>(std::move(line));
        PostMessageW(window, kMessageLog, 0, reinterpret_cast<LPARAM>(payload.release()));
    }

    void PostProgress(HWND window, const hazuki::PipelineProgress &progress)
    {
        auto payload = std::make_unique<ProgressPayload>();
        payload->completed = progress.completed;
        payload->total = progress.total;
        payload->current_item = progress.current_item;
        PostMessageW(window, kMessageProgress, 0, reinterpret_cast<LPARAM>(payload.release()));
    }

    void PostFinished(HWND window, bool success, bool auto_close, std::wstring message, std::filesystem::path output_dir)
    {
        auto payload = std::make_unique<FinishedPayload>();
        payload->success = success;
        payload->auto_close = auto_close;
        payload->message = std::move(message);
        payload->output_dir = std::move(output_dir);
        PostMessageW(window, kMessageFinished, 0, reinterpret_cast<LPARAM>(payload.release()));
    }

    void StartPipeline(AppState *state, const std::vector<std::filesystem::path> &inputs, bool auto_close)
    {
        if (inputs.empty())
            return;
        if (state->processing)
        {
            AppendLog(state, L"[BUSY] 当前有解包工作正在进行，请稍候。");
            return;
        }
        if (state->worker.joinable())
            state->worker.join();
        state->processing = true;
        state->auto_close = auto_close;
        state->has_result = false;
        state->last_run_success = false;
        state->completed = 0;
        state->total = inputs.size();
        state->current_item = L"正在初始化资源环境";
        state->status_message = L"准备解析文件...";
        AppendLog(state, std::wstring(L"[START] 已确认目标 ") + std::to_wstring(inputs.size()) + L" 个 ARC，等待注入。");
        const HWND window = state->window;
        state->worker = std::thread([window, inputs, auto_close]()
                                    {
        try {
            hazuki::PipelineOptions options;
            options.unpack_root = std::filesystem::current_path() / L"unpack";
            options.decode_codepage = 932;
            options.encode_codepage = 932;
            hazuki::PipelineCallbacks callbacks;
            callbacks.on_log = [window](const std::wstring &line) { PostLog(window, line); };
            callbacks.on_progress = [window](const hazuki::PipelineProgress &progress) { PostProgress(window, progress); };
            const auto result = hazuki::RunFullUnpackPipeline(inputs, options, callbacks);
            PostFinished(window, true, auto_close, std::wstring(L"资源分流结束。归档已输出至：") + result.unpack_root.wstring(), result.unpack_root);
        } catch (const std::exception &exception) {
            int wide_size = MultiByteToWideChar(CP_UTF8, 0, exception.what(), -1, nullptr, 0);
            std::wstring message = L"中断。";
            if (wide_size > 0) {
                message.resize(static_cast<std::size_t>(wide_size - 1));
                MultiByteToWideChar(CP_UTF8, 0, exception.what(), -1, message.data(), wide_size);
            }
            PostFinished(window, false, auto_close, message, {});
        } });
    }

    std::vector<std::filesystem::path> GatherDroppedPaths(HDROP drop)
    {
        std::vector<std::filesystem::path> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            path.resize(length);
            paths.emplace_back(path);
        }
        return paths;
    }

    std::vector<std::filesystem::path> PickArcFiles(HWND owner)
    {
        std::vector<std::filesystem::path> files;
        std::vector<wchar_t> buffer(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrFilter = L"BGI ARC 归档\0*.arc\0所有文件\0*.*\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrTitle = L"导入 ARC";
        if (!GetOpenFileNameW(&dialog))
            return files;
        const std::wstring directory = buffer.data();
        const wchar_t *cursor = buffer.data() + directory.size() + 1;
        if (*cursor == L'\0')
        {
            files.emplace_back(directory);
            return files;
        }
        while (*cursor != L'\0')
        {
            files.emplace_back(std::filesystem::path(directory) / cursor);
            cursor += std::wcslen(cursor) + 1;
        }
        return files;
    }

    void OpenOutputDirectory(AppState *state)
    {
        if (state->last_output_dir.empty())
            return;
        ShellExecuteW(state->window, L"open", state->last_output_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    AutoRunConfig ParseCommandLine()
    {
        AutoRunConfig config;
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return config;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring arg = argv[index];
            if (arg == L"--auto-close")
                config.auto_close = true;
            else
                config.inputs.emplace_back(arg);
        }
        LocalFree(argv);
        return config;
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto *state = reinterpret_cast<AppState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message)
        {
        case WM_CREATE:
        {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            state = reinterpret_cast<AppState *>(create->lpCreateParams);
            state->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&state->gdiplus_token, &input, nullptr);
            LoadAssets(state);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            state->client_width = rc.right - rc.left;
            state->client_height = rc.bottom - rc.top;
            DragAcceptFiles(hwnd, TRUE);
            AppendLog(state, L"[START] 环境初始化完毕。等待作业...");
            return 0;
        }
        case WM_SIZE:
            if (state)
            {
                state->client_width = LOWORD(lparam);
                state->client_height = HIWORD(lparam);
                state->log_offset = std::clamp(state->log_offset, 0, MaxLogOffset(state));
            }
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
            info->ptMinTrackSize.x = 640;
            info->ptMinTrackSize.y = 660;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (state)
            {
                POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const auto target = HitTest(state, point);
                if (target != state->hot_target)
                {
                    state->hot_target = target;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                if (!state->mouse_tracking)
                {
                    TRACKMOUSEEVENT event{};
                    event.cbSize = sizeof(event);
                    event.dwFlags = TME_LEAVE;
                    event.hwndTrack = hwnd;
                    TrackMouseEvent(&event);
                    state->mouse_tracking = true;
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            if (state)
            {
                state->mouse_tracking = false;
                state->hot_target = HitTarget::none;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (state && LOWORD(lparam) == HTCLIENT && state->hot_target != HitTarget::none && state->hot_target != HitTarget::drop_zone)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (state)
            {
                POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                state->pressed_target = HitTest(state, point);
                if (state->pressed_target != HitTarget::none)
                {
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (state)
            {
                POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const auto released = HitTest(state, point);
                const auto pressed = state->pressed_target;
                state->pressed_target = HitTarget::none;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (released == pressed)
                {
                    if (pressed == HitTarget::drop_zone || pressed == HitTarget::choose_button)
                        StartPipeline(state, PickArcFiles(hwnd), false);
                    else if (pressed == HitTarget::output_button)
                        OpenOutputDirectory(state);
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (state)
            {
                POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd, &point);
                if (ContainsPoint(ComputeLayout(state).log_view, point))
                {
                    state->log_offset = std::clamp(state->log_offset - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 3, 0, MaxLogOffset(state));
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        case WM_DROPFILES:
            if (state)
            {
                const auto files = GatherDroppedPaths(reinterpret_cast<HDROP>(wparam));
                DragFinish(reinterpret_cast<HDROP>(wparam));
                StartPipeline(state, files, false);
            }
            return 0;
        case kMessageLog:
            if (state)
            {
                std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring *>(lparam));
                if (payload)
                    AppendLog(state, *payload);
            }
            return 0;
        case kMessageProgress:
            if (state)
            {
                std::unique_ptr<ProgressPayload> payload(reinterpret_cast<ProgressPayload *>(lparam));
                if (payload)
                {
                    state->completed = payload->completed;
                    state->total = payload->total;
                    state->current_item = payload->current_item.empty() ? L"解析目录树结构..." : payload->current_item;
                    state->status_message = L"正在进行提取与分流";
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case kMessageFinished:
            if (state)
            {
                std::unique_ptr<FinishedPayload> payload(reinterpret_cast<FinishedPayload *>(lparam));
                if (payload)
                {
                    state->processing = false;
                    state->has_result = true;
                    if (state->worker.joinable())
                        state->worker.join();
                    state->last_run_success = payload->success;
                    state->last_output_dir = payload->output_dir;
                    if (payload->success && state->total > 0)
                        state->completed = state->total;
                    state->status_message = payload->success ? L"归档与重排已结束" : L"作业意外中断";
                    state->current_item = payload->success ? L"解包分流完成" : L"中断异常";
                    state->note = payload->message;
                    AppendLog(state, payload->success ? std::wstring(L"[DONE] ") + payload->message : std::wstring(L"[FAILED] ") + payload->message);
                    if (payload->auto_close && payload->success)
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_PAINT:
            if (state)
            {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                PaintUi(state, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (state && state->processing)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (state)
            {
                if (state->worker.joinable())
                    state->worker.join();
                state->private_fonts.reset();
                state->background_bitmap.reset();
                state->icon_bitmap.reset();
                if (state->gdiplus_token)
                    Gdiplus::GdiplusShutdown(state->gdiplus_token);
            }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    AppState state;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    // Config icon!
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));

    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(0, kWindowClassName, L"BGI Hazuki 解包",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  960, 720, nullptr, nullptr, instance, &state);
    if (!window)
        return 1;

    ShowWindow(window, show_command == 0 ? SW_SHOWDEFAULT : show_command);
    UpdateWindow(window);

    const auto auto_run = ParseCommandLine();
    if (!auto_run.inputs.empty())
        StartPipeline(&state, auto_run.inputs, auto_run.auto_close);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}