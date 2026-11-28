#include "rc/frame.h"

#include <algorithm>
#include <cstring>

namespace rc {
namespace {

inline int DivideRoundUp(int value, int unit) {
    return (value + unit - 1) / unit;
}

}  // namespace

FrameEncoder::FrameEncoder(uint16_t tile_size)
    : tile_size_(tile_size ? tile_size : kDefaultTileSize) {}

bool FrameEncoder::TileDiffers(int x, int y, int w, int h, int stride,
                               const uint8_t* pixels) const {
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4;
    for (int row = 0; row < h; ++row) {
        const std::size_t off = static_cast<std::size_t>(y + row) *
                                    static_cast<std::size_t>(stride) +
                                static_cast<std::size_t>(x) * 4;
        if (std::memcmp(pixels + off, prev_.data() + off, row_bytes) != 0) {
            return true;
        }
    }
    return false;
}

EncodedFrame FrameEncoder::Encode(const FrameImage& image) {
    EncodedFrame out;
    if (image.width <= 0 || image.height <= 0 || !image.pixels) return out;

    const int stride = image.width * 4;
    const int tiles_x = DivideRoundUp(image.width, tile_size_);
    const int tiles_y = DivideRoundUp(image.height, tile_size_);
    const bool geometry_changed =
        !has_prev_ || image.width != prev_w_ || image.height != prev_h_;

    out.key_frame = geometry_changed;
    out.tiles_total = tiles_x * tiles_y;

    struct DirtyTile {
        uint16_t index;
        uint16_t width;
        uint16_t height;
        const uint8_t* src;
    };
    std::vector<DirtyTile> dirty;
    dirty.reserve(static_cast<std::size_t>(out.tiles_total));

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            const int x0 = tx * tile_size_;
            const int y0 = ty * tile_size_;
            const int tw = std::min<int>(tile_size_, image.width - x0);
            const int th = std::min<int>(tile_size_, image.height - y0);
            if (!geometry_changed &&
                !TileDiffers(x0, y0, tw, th, stride, image.pixels)) {
                continue;
            }
            dirty.push_back(DirtyTile{
                static_cast<uint16_t>(ty * tiles_x + tx),
                static_cast<uint16_t>(tw),
                static_cast<uint16_t>(th),
                image.pixels + static_cast<std::size_t>(y0) *
                                   static_cast<std::size_t>(stride) +
                    static_cast<std::size_t>(x0) * 4,
            });
        }
    }

    out.tiles_dirty = static_cast<int>(dirty.size());
    out.frame_id = ++frame_counter_;

    const std::size_t table_bytes = dirty.size() * sizeof(TileEntry);
    const std::size_t data_offset = sizeof(FrameHead) + table_bytes;
    std::size_t data_bytes = 0;
    for (const auto& tile : dirty) {
        data_bytes += static_cast<std::size_t>(tile.width) * tile.height * 4;
    }
    out.raw_bytes = data_bytes;

    out.body.resize(data_offset + data_bytes);

    FrameHead head{};
    head.frame_id = out.frame_id;
    head.screen_w = static_cast<uint16_t>(image.width);
    head.screen_h = static_cast<uint16_t>(image.height);
    head.tile_size = tile_size_;
    head.tiles_x = static_cast<uint16_t>(tiles_x);
    head.tiles_y = static_cast<uint16_t>(tiles_y);
    head.tile_count = static_cast<uint16_t>(dirty.size());
    // 光标坐标按 int16 传输，先夹紧到画面范围内，避免越界产生歧义。
    head.cursor_x = static_cast<int16_t>(
        std::clamp(image.cursor_x, 0, image.width > 0 ? image.width - 1 : 0));
    head.cursor_y = static_cast<int16_t>(
        std::clamp(image.cursor_y, 0, image.height > 0 ? image.height - 1 : 0));
    head.cursor_visible = image.cursor_visible ? 1 : 0;
    head.key_frame = geometry_changed ? 1 : 0;

    std::memcpy(out.body.data(), &head, sizeof(head));

    uint8_t* cursor = out.body.data() + sizeof(FrameHead);
    for (const auto& tile : dirty) {
        TileEntry entry{};
        entry.tile_index = tile.index;
        entry.width = tile.width;
        entry.height = tile.height;
        entry.data_len = static_cast<uint32_t>(tile.width) * tile.height * 4u;
        std::memcpy(cursor, &entry, sizeof(entry));
        cursor += sizeof(entry);
    }

    // 像素数据紧跟分块表。逐行拷贝，保证目标紧凑排列（源 stride 可能更大）。
    std::size_t offset = data_offset;
    for (const auto& tile : dirty) {
        const std::size_t row_bytes = static_cast<std::size_t>(tile.width) * 4;
        for (int row = 0; row < tile.height; ++row) {
            std::memcpy(out.body.data() + offset,
                        tile.src + static_cast<std::size_t>(row) *
                                       static_cast<std::size_t>(stride),
                        row_bytes);
            offset += row_bytes;
        }
    }

    // 维护"上一帧"：整帧变化时整体替换，否则只覆盖脏块。
    if (geometry_changed) {
        prev_.assign(image.pixels,
                     image.pixels + static_cast<std::size_t>(stride) *
                                        static_cast<std::size_t>(image.height));
        prev_w_ = image.width;
        prev_h_ = image.height;
        has_prev_ = true;
    } else {
        for (const auto& tile : dirty) {
            const int tx = tile.index % tiles_x;
            const int ty = tile.index / tiles_x;
            const std::size_t row_bytes =
                static_cast<std::size_t>(tile.width) * 4;
            for (int row = 0; row < tile.height; ++row) {
                const std::size_t off =
                    static_cast<std::size_t>(ty * tile_size_ + row) *
                        static_cast<std::size_t>(stride) +
                    static_cast<std::size_t>(tx * tile_size_) * 4;
                std::memcpy(prev_.data() + off,
                            tile.src + static_cast<std::size_t>(row) *
                                           static_cast<std::size_t>(stride),
                            row_bytes);
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// FrameDecoder
// ---------------------------------------------------------------------------

bool FrameDecoder::Begin(const uint8_t* body, std::size_t len) {
    if (!body || len < sizeof(FrameHead)) return false;

    FrameHead head{};
    std::memcpy(&head, body, sizeof(head));

    if (head.screen_w == 0 || head.screen_h == 0 || head.tile_size == 0 ||
        head.tiles_x == 0 || head.tiles_y == 0) {
        return false;
    }

    const std::size_t table_bytes =
        static_cast<std::size_t>(head.tile_count) * sizeof(TileEntry);
    if (len < sizeof(FrameHead) + table_bytes) return false;

    head_.frame_id = head.frame_id;
    head_.screen_w = head.screen_w;
    head_.screen_h = head.screen_h;
    head_.tile_size = head.tile_size;
    head_.tiles_x = head.tiles_x;
    head_.tiles_y = head.tiles_y;
    head_.tile_count = head.tile_count;
    head_.cursor_x = head.cursor_x;
    head_.cursor_y = head.cursor_y;
    head_.cursor_visible = head.cursor_visible != 0;  //从整数转成 bool（!= 0），因为协议里用 uint8_t 传布尔值。
    key_frame_ = head.key_frame != 0;

    tiles_ = body + sizeof(FrameHead);
    pixels_ = tiles_ + table_bytes;

    // 校验分块表与数据区一致，防止越界写入客户端表面。
    const std::size_t avail = len - sizeof(FrameHead) - table_bytes;
    std::size_t needed = 0;
    for (int i = 0; i < head_.tile_count; ++i) {
        TileEntry entry{};
        std::memcpy(&entry, tiles_ + static_cast<std::size_t>(i) * sizeof(TileEntry),
                    sizeof(entry));
        if (entry.tile_index >= static_cast<uint16_t>(head_.tiles_x * head_.tiles_y)) {
            return false;
        }
        const int tx = entry.tile_index % head_.tiles_x;
        const int ty = entry.tile_index / head_.tiles_x;
        if (tx * head_.tile_size + entry.width > head_.screen_w) return false;
        if (ty * head_.tile_size + entry.height > head_.screen_h) return false;
        if (entry.data_len !=
            static_cast<uint32_t>(entry.width) * entry.height * 4u) {
            return false;
        }
        needed += entry.data_len;
        if (needed > avail) return false;
    }
    return needed == avail;
}

bool FrameDecoder::BlitTo(const Target& target) const {
    if (!target.pixels || !target.stride) return false;
    if (static_cast<int>(target.stride) < head_.screen_w * 4) return false;

    std::size_t offset = 0;
    for (int i = 0; i < head_.tile_count; ++i) {
        TileEntry entry{};
        std::memcpy(&entry, tiles_ + static_cast<std::size_t>(i) * sizeof(TileEntry),
                    sizeof(entry));
        const int tx = entry.tile_index % head_.tiles_x;
        const int ty = entry.tile_index / head_.tiles_x;
        const int x0 = tx * head_.tile_size;
        const int y0 = ty * head_.tile_size;
        const std::size_t row_bytes = static_cast<std::size_t>(entry.width) * 4;
        for (int row = 0; row < entry.height; ++row) {
            uint8_t* dst = target.pixels +
                           static_cast<std::size_t>(y0 + row) * target.stride +
                           static_cast<std::size_t>(x0) * 4;
            std::memcpy(dst, pixels_ + offset, row_bytes);
            offset += row_bytes;
        }
    }
    return true;
}

}  // namespace rc
