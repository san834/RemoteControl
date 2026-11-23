// 分块脏矩形帧编解码。
//
// 旧实现：每次请求都 BitBlt 全屏 → PNG 编码整帧 → 发送整帧。
//   1080p 的 PNG 通常在 300KB~2MB 之间，而且锁步请求-响应意味着
//   帧率被"捕获 + 全屏 PNG 编码"的时间卡死。
//
// 新实现：
//   1. 把画面切成 64x64 的块，逐块与上一帧比较，只编码有变化的块；
//   2. 组装成"帧头 + 分块表 + 像素"，再整体交给 MSZIP 压缩；
//   3. 上一帧的像素只更新脏块，避免每帧 memcpy 整屏。
//
// 帧体布局（未压缩时）：
//   FrameHead | TileEntry * tile_count | 各块的 BGRA 像素（按表顺序紧密排列）
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rc/protocol.h"

namespace rc {

// 一帧的输入视图。pixels 必须是 BGRA8888、自上而下、stride = width * 4。
struct FrameImage {
    int width = 0;
    int height = 0;
    const uint8_t* pixels = nullptr;
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = false;
};

struct EncodedFrame {
    std::vector<uint8_t> body;  // 未压缩的帧体
    uint32_t frame_id = 0;
    bool key_frame = false;
    int tiles_total = 0;
    int tiles_dirty = 0;
    std::size_t raw_bytes = 0;  // 本帧实际搬运的像素字节数

    bool empty() const { return body.empty(); }
};

class FrameEncoder {
public:
    explicit FrameEncoder(uint16_t tile_size = kDefaultTileSize);

    // 编码一帧。第一帧、以及分辨率变化时自动变成关键帧。
    EncodedFrame Encode(const FrameImage& image);

    // 强制下一帧为关键帧（例如客户端重连后要整屏刷新）。
    void RequestKeyFrame() { has_prev_ = false; }

    uint16_t tile_size() const { return tile_size_; }

private:
    bool TileDiffers(int x, int y, int w, int h, int stride,
                     const uint8_t* pixels) const;

    uint16_t tile_size_;
    int prev_w_ = 0;
    int prev_h_ = 0;
    bool has_prev_ = false;
    uint32_t frame_counter_ = 0;
    std::vector<uint8_t> prev_;
};

struct FrameHeadView {
    uint32_t frame_id = 0;
    int screen_w = 0;
    int screen_h = 0;
    int tile_size = 0;
    int tiles_x = 0;
    int tiles_y = 0;
    int tile_count = 0;
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = false;
};

// 客户端侧的解码器：先 Begin() 校验并解析帧头，拿到分辨率后
// 由调用方准备好目标表面，再 BlitTo() 把脏块写入。
class FrameDecoder {
public:
    // 第二阶段的输出目标。
    struct Target {
        uint8_t* pixels = nullptr;  // BGRA8888
        std::size_t stride = 0;     // 字节数
    };

    bool Begin(const uint8_t* body, std::size_t len);
    bool BlitTo(const Target& target) const;

    const FrameHeadView& head() const { return head_; }
    bool key_frame() const { return key_frame_; }

private:
    FrameHeadView head_;
    bool key_frame_ = false;
    const uint8_t* tiles_ = nullptr;   // TileEntry 数组
    const uint8_t* pixels_ = nullptr;  // 像素数据起始
};

}  // namespace rc
