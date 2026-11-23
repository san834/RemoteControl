// 屏幕捕获抽象。
//
// 抽象出接口的目的：换个后端（DXGI Desktop Duplication / Windows Graphics
// Capture）不需要动会话和协议层。当前提供的是兼容性最好的 GDI BitBlt 实现。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rc {

struct CapturedFrame {
    int width = 0;
    int height = 0;
    int origin_x = 0;  // 虚拟桌面左上角在屏幕坐标系的偏移（多显示器）
    int origin_y = 0;
    int scale = 1;     // 降采样倍数：远端坐标 = 帧坐标 * scale + origin
    int cursor_x = 0;  // 相对捕获图像的坐标
    int cursor_y = 0;
    bool cursor_visible = false;
    std::vector<uint8_t> pixels;  // BGRA8888，stride = width * 4
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // 捕获当前桌面。scale > 1 时按整数倍降采样（弱网/高分屏下省带宽）。
    // 失败返回 false 并填充 error。
    virtual bool Capture(CapturedFrame& out, int scale, char* error,
                         std::size_t error_size) = 0;

    virtual const char* name() const = 0;
};

// GDI 后端：GetDC(NULL) + BitBlt 到 DIB 段。
// 句柄在实例内复用，避免每帧 CreateCompatibleDC / CreateDIBSection。
std::unique_ptr<IScreenCapture> CreateGdiCapture();

}  // namespace rc
