#include "rc/stream.h"

#include <cstring>

#include "rc/crc32.h"

namespace rc {
namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

}  // namespace

std::vector<uint8_t> Encode(Cmd cmd, uint32_t seq, uint16_t flags,
                            const void* body, std::size_t body_len) {
    std::vector<uint8_t> out(kHeaderSize + body_len);
    PacketHeader header{};
    header.magic = kMagic;
    header.cmd = static_cast<uint16_t>(cmd);
    header.flags = flags;
    header.body_len = static_cast<uint32_t>(body_len);
    header.seq = seq;
    header.crc32 = body_len ? Crc32(body, body_len) : 0;
    std::memcpy(out.data(), &header, kHeaderSize);
    if (body_len) std::memcpy(out.data() + kHeaderSize, body, body_len);
    return out;
}

Decoder::Decoder(std::size_t max_body)
    : max_body_(max_body ? max_body : kMaxBodyBytes) {}

void Decoder::Reset() {
    buf_.clear();
    read_ = 0;
    ready_.clear();
}

void Decoder::Consume(std::size_t n) {
    read_ += n;
    if (read_ >= buf_.size()) {
        buf_.clear();
        read_ = 0;
        return;
    }
    // 定期把已消费的部分真正丢掉，避免 vector 无限增长。摊还成本 O(1)。
    if (read_ >= (1u << 20) || read_ * 2 >= buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(read_));
        read_ = 0;
    }
}

std::size_t Decoder::FindMagic(std::size_t from) const {
    if (buf_.size() < 4) return kNpos;
    const auto* p = buf_.data();
    const std::size_t last = buf_.size() - 4;
    for (std::size_t i = from; i <= last; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, p + i, sizeof(v));
        if (v == kMagic) return i;
    }
    return kNpos;
}

void Decoder::Push(const uint8_t* data, std::size_t len) {
    if (!len) return;
    buf_.insert(buf_.end(), data, data + len);

    for (;;) {
        const std::size_t avail = Buffered();
        if (avail < kHeaderSize) return;

        PacketHeader header{};
        std::memcpy(&header, buf_.data() + read_, kHeaderSize);

        // 头部不可信：magic 不对，或长度越界。滑动到下一个可能的包头。
        if (header.magic != kMagic || header.body_len > max_body_) {
            ++stats_.resyncs;
            std::size_t next = FindMagic(read_ + 1);
            std::size_t drop;
            if (next == kNpos) {
                // 找不到就不再多留：保留末尾 3 字节，防止魔数被本次
                // Push 的边界切成两半。
                drop = Buffered() > 3 ? Buffered() - 3 : 0;
            } else {
                drop = next - read_;
            }
            stats_.dropped_bytes += drop;
            if (drop == 0) return;  // 没有进展，等更多数据
            Consume(drop);
            continue;
        }

        if (avail < kHeaderSize + header.body_len) return;  // 半包，等补齐

        const uint8_t* body = buf_.data() + read_ + kHeaderSize;
        if (header.body_len &&
            Crc32(body, header.body_len) != header.crc32) {
            ++stats_.crc_mismatch;
            ++stats_.resyncs;
            stats_.dropped_bytes += 1;
            Consume(1);  // 跳过 1 字节后重新同步
            continue;
        }

        Message msg;
        // cmd 来自网络，收敛到已知枚举之外时仍原样保留，由上层忽略。
        msg.cmd = static_cast<Cmd>(header.cmd);
        msg.flags = header.flags;
        msg.seq = header.seq;
        msg.body.assign(body, body + header.body_len);
        ready_.push_back(std::move(msg));

        ++stats_.packets;
        Consume(kHeaderSize + header.body_len);
    }
}

bool Decoder::Pop(Message& out) {
    if (ready_.empty()) return false;
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

}  // namespace rc
