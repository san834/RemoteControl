#include "rc/compress.h"

#include <windows.h>
#include <compressapi.h>

namespace rc {
namespace {

constexpr int kMaxRetries = 8;
constexpr int kAlgorithmCount = 3;  // 按 Compression 枚举值索引

// 压缩/解压句柄的线程局部缓存。
//
// 用"句柄 + Compress/Decompress"而不是一次性 API：句柄版本自 Win8 起稳定
// 存在。缓存句柄避免每帧创建/销毁（每次创建都要分配几百 KB 的窗口缓冲）。
struct HandleCache {
    COMPRESSOR_HANDLE handle = nullptr;
    ~HandleCache() {
        if (handle) CloseCompressor(handle);
    }
};

struct DecompressorCache {
    DECOMPRESSOR_HANDLE handle = nullptr;
    ~DecompressorCache() {
        if (handle) CloseDecompressor(handle);
    }
};

thread_local HandleCache g_compressors[kAlgorithmCount];
thread_local DecompressorCache g_decompressors[kAlgorithmCount];

DWORD ToApiAlgorithm(Compression algorithm) {
    return algorithm == Compression::kXpressHuff
               ? COMPRESS_ALGORITHM_XPRESS_HUFF
               : COMPRESS_ALGORITHM_MSZIP;
}

COMPRESSOR_HANDLE CompressorFor(Compression algorithm) {
    const int index = static_cast<int>(algorithm);
    if (index <= 0 || index >= kAlgorithmCount) return nullptr;
    auto& cache = g_compressors[index];
    if (!cache.handle) {
        CreateCompressor(ToApiAlgorithm(algorithm), nullptr, &cache.handle);
    }
    return cache.handle;
}

DECOMPRESSOR_HANDLE DecompressorFor(Compression algorithm) {
    const int index = static_cast<int>(algorithm);
    if (index <= 0 || index >= kAlgorithmCount) return nullptr;
    auto& cache = g_decompressors[index];
    if (!cache.handle) {
        CreateDecompressor(ToApiAlgorithm(algorithm), nullptr, &cache.handle);
    }
    return cache.handle;
}

}  // namespace

bool CompressionAvailable(Compression algorithm) {
    COMPRESSOR_HANDLE handle = nullptr;
    if (!CreateCompressor(ToApiAlgorithm(algorithm), nullptr, &handle)) {
        return false;
    }
    CloseCompressor(handle);
    return true;
}

Compression PreferredCompression() {
    static const Compression preferred = [] {
        if (CompressionAvailable(Compression::kXpressHuff)) {
            return Compression::kXpressHuff;
        }
        if (CompressionAvailable(Compression::kMsZip)) {
            return Compression::kMsZip;
        }
        return Compression::kNone;
    }();
    return preferred;
}

bool CompressData(const void* src, std::size_t len, Compression algorithm,
                  std::vector<uint8_t>& out) {
    out.clear();
    if (!src || !len) return false;
    COMPRESSOR_HANDLE handle = CompressorFor(algorithm);
    if (!handle) return false;

    // 一次性给足空间：如果第一次就 ERROR_INSUFFICIENT_BUFFER，API 会要求把
    // 整个输入重新压一遍，代价直接翻倍。多留 25% + 4KB 基本一次成功。
    std::size_t cap = len + len / 4 + 4096;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        out.resize(cap);
        SIZE_T produced = static_cast<SIZE_T>(cap);
        if (::Compress(handle, src, static_cast<SIZE_T>(len), out.data(),
                       static_cast<SIZE_T>(cap), &produced)) {
            out.resize(produced);
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            out.clear();
            return false;
        }
        cap = produced > cap ? produced : cap * 2;
    }
    out.clear();
    return false;
}

bool DecompressData(const void* src, std::size_t len, Compression algorithm,
                    std::size_t max_out, std::vector<uint8_t>& out) {
    out.clear();
    if (!src || !len || !max_out) return false;
    DECOMPRESSOR_HANDLE handle = DecompressorFor(algorithm);
    if (!handle) return false;

    std::size_t cap = len * 6 + 4096;
    if (cap > max_out) cap = max_out;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        out.resize(cap);
        SIZE_T produced = static_cast<SIZE_T>(cap);
        if (::Decompress(handle, src, static_cast<SIZE_T>(len), out.data(),
                         static_cast<SIZE_T>(cap), &produced)) {
            out.resize(produced);
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            out.clear();
            return false;
        }
        if (produced > max_out) {  // 伪造的长度：拒绝，不分配
            out.clear();
            return false;
        }
        const std::size_t grown = produced > cap ? produced : cap * 2;
        if (grown > max_out) {
            if (cap >= max_out) {
                out.clear();
                return false;
            }
            cap = max_out;
        } else {
            cap = grown;
        }
    }
    out.clear();
    return false;
}

}  // namespace rc
