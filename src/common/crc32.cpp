#include "rc/crc32.h"

#include <array>

namespace rc {
namespace {

// 运行时生成查找表，避免把 1KB 常量表塞进源码。C++11 起静态局部量的初始化
// 是线程安全的，因此这里不需要额外加锁。
const std::array<uint32_t, 256>& Table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

uint32_t Crc32(const void* data, std::size_t len, uint32_t seed) {
    const auto& table = Table();
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace rc
