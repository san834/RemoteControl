#include "capture.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rc {
namespace {

void SetError(char* error, std::size_t size, const char* message) {
    if (!error || !size) return;
    std::snprintf(error, size, "%s", message);
}

class GdiCapture final : public IScreenCapture {
public:
    ~GdiCapture() override { ReleaseSurface(); }

    bool Capture(CapturedFrame& out, int scale, char* error,
                 std::size_t error_size) override {
        if (scale < 1) scale = 1;

        const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int full_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int full_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (full_w <= 0 || full_h <= 0) {
            SetError(error, error_size, "无法获取虚拟桌面尺寸");
            return false;
        }

        const int width = std::max(1, full_w / scale);
        const int height = std::max(1, full_h / scale);

        if (!EnsureSurface(width, height)) {
            SetError(error, error_size, "创建 DIB 表面失败");
            return false;
        }

        HDC screen = GetDC(nullptr);
        if (!screen) {
            SetError(error, error_size, "GetDC(NULL) 失败");
            return false;
        }

        BOOL ok = FALSE;
        if (scale == 1) {
            // CAPTUREBLT 让分层窗口（部分播放器/游戏）也能被抓到。
            ok = BitBlt(mem_dc_, 0, 0, width, height, screen, origin_x,
                        origin_y, SRCCOPY | CAPTUREBLT);
        } else {
            // 降采样：让 GDI 的 HALFTONE 缩放器一次性完成，比"全尺寸抓取
            // 再自己缩"少一次全屏内存往返。
            const int old_mode = SetStretchBltMode(mem_dc_, HALFTONE);
            SetBrushOrgEx(mem_dc_, 0, 0, nullptr);
            ok = StretchBlt(mem_dc_, 0, 0, width, height, screen, origin_x,
                            origin_y, full_w, full_h, SRCCOPY);
            SetStretchBltMode(mem_dc_, old_mode);
        }
        ReleaseDC(nullptr, screen);

        if (!ok) {
            SetError(error, error_size, "截屏失败（BitBlt/StretchBlt）");
            return false;
        }

        out.width = width;
        out.height = height;
        out.origin_x = origin_x;
        out.origin_y = origin_y;
        out.scale = scale;
        out.pixels.resize(static_cast<std::size_t>(width) * height * 4);

        const auto* src = static_cast<const uint8_t*>(bits_);
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                out.pixels.data() + static_cast<std::size_t>(row) * width * 4,
                src + static_cast<std::size_t>(row) * stride_,
                static_cast<std::size_t>(width) * 4);
        }

        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING)) {
            out.cursor_visible = true;
            out.cursor_x = (cursor.ptScreenPos.x - origin_x) / scale;
            out.cursor_y = (cursor.ptScreenPos.y - origin_y) / scale;
        } else {
            out.cursor_visible = false;
            out.cursor_x = 0;
            out.cursor_y = 0;
        }
        return true;
    }

    const char* name() const override { return "GDI BitBlt"; }

private:
    bool EnsureSurface(int width, int height) {
        if (mem_dc_ && width == width_ && height == height_) return true;

        ReleaseSurface();

        HDC screen = GetDC(nullptr);
        if (!screen) return false;
        mem_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!mem_dc_) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        // 负高度 = 自上而下的 DIB，和协议里的像素顺序一致。
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dib_ =
            CreateDIBSection(mem_dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib_ || !bits) {
            ReleaseSurface();
            return false;
        }
        SelectObject(mem_dc_, dib_);
        bits_ = bits;
        stride_ = width * 4;
        width_ = width;
        height_ = height;
        return true;
    }

    void ReleaseSurface() {
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (mem_dc_) {
            DeleteDC(mem_dc_);
            mem_dc_ = nullptr;
        }
        bits_ = nullptr;
        stride_ = 0;
        width_ = 0;
        height_ = 0;
    }

    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int stride_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace

std::unique_ptr<IScreenCapture> CreateGdiCapture() {
    return std::make_unique<GdiCapture>();
}

}  // namespace rc
