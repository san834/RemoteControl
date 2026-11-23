// 流式编解码器。
//
// 旧实现的问题：把整个缓冲区交给 ParsePacket，让它"扫描魔数"定位包头，
// 但返回的 Packet 不带偏移量，调用方一律假设包头在 buffer[0]，一旦出现脏
// 字节就永久错位；而且每个包都要线性扫一遍，退化成 O(n^2)。
//
// 这里的做法：TCP 是字节流，包头定长 20 字节，因此正常情况下只需按状态
// 依次消费，不需要扫描。只有在包头校验失败（magic 不匹配 / body_len 越界 /
// CRC 不符）时才进入"重同步"分支，向后滑动寻找下一个合法包头。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "rc/protocol.h"

namespace rc {

// 一条完整的逻辑消息（包体已按需解压）。
struct Message {
    Cmd cmd = Cmd::kHello;
    uint16_t flags = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> body;

    template <typename T>
    bool As(T& out) const {
        if (body.size() < sizeof(T)) return false;
        // 逐字节拷贝：包体是字节流，不能假设天然对齐。
        std::memcpy(&out, body.data(), sizeof(T));
        return true;
    }
};

// 把一条消息序列化成完整的字节块（包头 + 包体）。
// body 为空时允许传 nullptr / 0。
std::vector<uint8_t> Encode(Cmd cmd, uint32_t seq, uint16_t flags,
                            const void* body, std::size_t body_len);

// 解包器统计，用于诊断和单元测试。
struct DecoderStats {
    uint64_t packets = 0;         // 成功解出的包数
    uint64_t resyncs = 0;         // 进入重同步的次数
    uint64_t dropped_bytes = 0;   // 重同步丢弃的字节数
    uint64_t crc_mismatch = 0;    // CRC 校验失败次数
};

class Decoder {
public:
    explicit Decoder(std::size_t max_body = kMaxBodyBytes);

    // 追加一段收到的原始字节。允许任意切分（1 字节也可以）。
    void Push(const uint8_t* data, std::size_t len);

    // 取出下一条完整消息；没有则返回 false。
    bool Pop(Message& out);

    std::size_t Pending() const { return ready_.size(); }
    std::size_t Buffered() const { return buf_.size() - read_; }
    const DecoderStats& stats() const { return stats_; }

    void Reset();

private:
    // 丢弃缓冲区头部的 n 字节。
    void Consume(std::size_t n);
    // 从 from 开始寻找下一个可能的包头起始位置，找不到返回 npos。
    std::size_t FindMagic(std::size_t from) const;

    std::vector<uint8_t> buf_;
    std::size_t read_ = 0;
    std::deque<Message> ready_;
    std::size_t max_body_;
    DecoderStats stats_;
};

}  // namespace rc
