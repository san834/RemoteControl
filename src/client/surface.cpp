#include "surface.h"

#include <algorithm>

namespace rc {

Surface::~Surface() { Release(); }

void Surface::Release() {
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    if (mem_dc_) {
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
}

bool Surface::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (dib_ && width == width_ && height == height_) return true;

    Release();

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    mem_dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!mem_dc_) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 负值 = 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(mem_dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_ || !bits) {
        Release();
        return false;
    }
    SelectObject(mem_dc_, dib_);
    bits_ = bits;
    width_ = width;
    height_ = height;
    stride_ = static_cast<std::size_t>(width) * 4;
    return true;
}

bool Surface::Blit(HDC dest, int x, int y, int w, int h) const {
    if (!dib_ || !mem_dc_ || w <= 0 || h <= 0) return false;

    // HALFTONE 在缩小时明显比 COLORONCOLOR 干净；配合 SetBrushOrgEx 重置
    // 画刷原点，否则会出现错位的 2x2 棋盘纹。
    const int old_mode = SetStretchBltMode(dest, HALFTONE);
    SetBrushOrgEx(dest, 0, 0, nullptr);
    const BOOL ok = StretchBlt(dest, x, y, w, h, mem_dc_, 0, 0, width_, height_,
                               SRCCOPY);
    SetStretchBltMode(dest, old_mode);
    return ok != FALSE;
}

RECT FitRect(int client_w, int client_h, int src_w, int src_h) {
    RECT rect{0, 0, client_w, client_h};
    if (client_w <= 0 || client_h <= 0 || src_w <= 0 || src_h <= 0) return rect;

    const double scale = std::min(static_cast<double>(client_w) / src_w,
                                  static_cast<double>(client_h) / src_h);
    const int w = std::max(1, static_cast<int>(src_w * scale));
    const int h = std::max(1, static_cast<int>(src_h * scale));
    rect.left = (client_w - w) / 2;
    rect.top = (client_h - h) / 2;
    rect.right = rect.left + w;
    rect.bottom = rect.top + h;
    return rect;
}

}  // namespace rc
