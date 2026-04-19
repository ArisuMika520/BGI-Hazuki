#pragma once

#include "bgi_cbg_codec.h"

namespace hazuki {

class ImagingSession {
public:
    ImagingSession();
    ~ImagingSession();

    ImagingSession(const ImagingSession &) = delete;
    ImagingSession &operator=(const ImagingSession &) = delete;

private:
    unsigned long long token_ = 0;
};

RasterImage LoadPng(const std::filesystem::path &path);
void SavePng(const RasterImage &image, const std::filesystem::path &path);

}  // namespace hazuki
