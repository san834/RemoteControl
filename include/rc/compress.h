// 使用 Windows 内置的压缩 API（Cabinet.lib），不引入第三方依赖。
//
// 为什么不用 zlib：这个项目希望 clone 之后只装 VS 就能构建。
//
// 两种算法：XPRESS_HUFF（Win10+）压缩速度约为 MSZIP 的 4~8 倍，是屏幕
// 这种高熵数据的正确选择；MSZIP 兼容 Win8。启动时探测一次，选快的那个，
// 并把算法写进包头标志位，接收端按标志位解压 —— 这样新旧版本也能互通。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rc {

enum class Compression : uint8_t {
    kNone = 0,
    kMsZip = 1,
    kXpressHuff = 2,
};

// 探测一次并缓存：优先 XPRESS_HUFF，不可用时退回 MSZIP，都不可用返回 kNone。
Compression PreferredCompression();

bool CompressionAvailable(Compression algorithm);

bool CompressData(const void* src, std::size_t len, Compression algorithm,
                  std::vector<uint8_t>& out);

// max_out 是允许的输出上限，防止伪造的长度导致巨额分配。
bool DecompressData(const void* src, std::size_t len, Compression algorithm,
                    std::size_t max_out, std::vector<uint8_t>& out);

}  // namespace rc
