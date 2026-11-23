// 标准 IEEE 802.3 CRC32（多项式 0xEDB88320），用于包头校验。
#pragma once

#include <cstddef>
#include <cstdint>

namespace rc {

// 对 [data, data + len) 计算 CRC32。seed 允许增量计算：
//   crc = Crc32(a, n);
//   crc = Crc32(b, m, crc);
uint32_t Crc32(const void* data, std::size_t len, uint32_t seed = 0);

}  // namespace rc
