// 客户端渲染目标：一块持久化的 32bpp DIB 段。
//
// 为什么不每帧 CreateCompatibleBitmap：1080p 的 32bpp 位图是 8MB，每帧分配 +
// 释放会造成明显的 GDI 抖动；而且解码器需要直接往表面里写脏块，只有 DIB 段
// 才能拿到稳定的内存指针。
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rc {

class Surface {
public:
    Surface() = default;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool Resize(int width, int height);
    void Release();

    bool valid() const { return dib_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint8_t* bits() { return static_cast<uint8_t*>(bits_); }
    std::size_t stride() const { return stride_; }

    // 等比缩放到 dest DC 的 (x, y, w, h)。
    bool Blit(HDC dest, int x, int y, int w, int h) const;

private:
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::size_t stride_ = 0;
};

// 在客户区里按原始宽高比居中放大的目标矩形（两侧或上下留黑边）。
// 保持比例的意义：不做 letterbox 时远端画面会被拉伸，鼠标坐标换算也会
// 跟着失真。
RECT FitRect(int client_w, int client_h, int src_w, int src_h);

}  // namespace rc
