#include "png_io.h"

#include <Windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace hazuki {
namespace {

CLSID GetPngEncoderClsid() {
    UINT encoder_count = 0;
    UINT encoder_bytes = 0;
    Gdiplus::GetImageEncodersSize(&encoder_count, &encoder_bytes);
    if (!encoder_bytes) {
        throw std::runtime_error("GDI+ PNG encoder is unavailable.");
    }

    std::vector<std::uint8_t> buffer(encoder_bytes);
    auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    if (Gdiplus::GetImageEncoders(encoder_count, encoder_bytes, encoders) != Gdiplus::Ok) {
        throw std::runtime_error("Failed to query GDI+ encoders.");
    }

    for (UINT i = 0; i < encoder_count; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            return encoders[i].Clsid;
        }
    }

    throw std::runtime_error("PNG encoder was not found.");
}

void ThrowIfGdiPlusError(Gdiplus::Status status, const char *message) {
    if (status != Gdiplus::Ok) {
        throw std::runtime_error(message);
    }
}

}  // namespace

ImagingSession::ImagingSession() {
    Gdiplus::GdiplusStartupInput startup_input;
    ULONG_PTR token = 0;
    const auto status = Gdiplus::GdiplusStartup(&token, &startup_input, nullptr);
    ThrowIfGdiPlusError(status, "Failed to initialize GDI+.");
    token_ = static_cast<unsigned long long>(token);
}

ImagingSession::~ImagingSession() {
    if (token_) {
        Gdiplus::GdiplusShutdown(static_cast<ULONG_PTR>(token_));
    }
}

RasterImage LoadPng(const std::filesystem::path &path) {
    Gdiplus::Bitmap source(path.c_str());
    ThrowIfGdiPlusError(source.GetLastStatus(), "Failed to open PNG image.");

    const auto width = source.GetWidth();
    const auto height = source.GetHeight();
    if (width == 0 || height == 0) {
        throw std::runtime_error("PNG image has invalid dimensions.");
    }

    std::unique_ptr<Gdiplus::Bitmap> converted(
        source.Clone(0, 0, width, height, PixelFormat32bppARGB));
    if (!converted || converted->GetLastStatus() != Gdiplus::Ok) {
        throw std::runtime_error("Failed to convert PNG image to 32bpp ARGB.");
    }

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData data{};
    ThrowIfGdiPlusError(
        converted->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data),
        "Failed to lock PNG pixel buffer.");

    RasterImage image;
    image.width = width;
    image.height = height;
    image.has_alpha = false;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

    const auto *src_base = static_cast<const std::uint8_t *>(data.Scan0);
    const auto stride = data.Stride;
    for (UINT y = 0; y < height; ++y) {
        const auto *src_row = stride >= 0
            ? src_base + static_cast<std::size_t>(stride) * y
            : src_base + static_cast<std::size_t>(-stride) * (height - 1 - y);
        auto *dst_row = image.pixels.data() + static_cast<std::size_t>(y) * width * 4;
        std::copy(src_row, src_row + static_cast<std::size_t>(width) * 4, dst_row);
    }

    converted->UnlockBits(&data);

    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] != 0xFF) {
            image.has_alpha = true;
            break;
        }
    }

    return image;
}

void SavePng(const RasterImage &image, const std::filesystem::path &path) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        throw std::runtime_error("RasterImage is invalid.");
    }

    const auto png_clsid = GetPngEncoderClsid();
    Gdiplus::Bitmap bitmap(
        image.width,
        image.height,
        static_cast<INT>(image.width * 4),
        PixelFormat32bppARGB,
        const_cast<BYTE *>(reinterpret_cast<const BYTE *>(image.pixels.data())));
    ThrowIfGdiPlusError(bitmap.GetLastStatus(), "Failed to create GDI+ bitmap.");
    ThrowIfGdiPlusError(bitmap.Save(path.c_str(), &png_clsid, nullptr), "Failed to save PNG image.");
}

}  // namespace hazuki
