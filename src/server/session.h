// 单个客户端会话。
//
// 三流水线：
//   接收线程（调用方线程）  recv → 解码 → 分发（注入输入 / 置位帧请求）
//   捕获线程                等到帧请求 → 捕获 → 差分成帧 → 压缩 → 入队
//   发送线程                出队 → SendAll（socket 的唯一写入者）
//
// "唯一写入者"是这里最关键的约束：旧实现里客户端有两个线程同时 send 同一个
// socket，TCP 字节流会交错。现在所有出站数据都经过一个队列，由一个线程写。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "inject.h"
#include "rc/compress.h"
#include "rc/frame.h"
#include "rc/net.h"
#include "rc/queue.h"
#include "rc/stream.h"

namespace rc {

struct ServerConfig {
    uint16_t port = 9999;
    uint32_t token_hash = 0;  // 0 表示不启用口令
    uint16_t tile_size = kDefaultTileSize;
    int min_compress_bytes = 256;
    int max_fps = 0;  // 0 = 不限速
    int idle_timeout_ms = 8000;
    int scale = 1;  // 降采样倍数（1/2/3/4），越低越省带宽
    Compression codec = Compression::kNone;  // kNone = 自动选择
    bool verbose = false;
};

// 待发送的整包 + 统计信息。
struct OutPacket {
    std::vector<uint8_t> bytes;
    bool is_frame = false;
    int tiles = 0;
    std::size_t raw_bytes = 0;
};

class ServerSession {
public:
    ServerSession(net::Socket socket, std::string peer,
                  const ServerConfig& config, IScreenCapture* capture,
                  IInputInjector* injector);
    ~ServerSession();

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    // 阻塞运行直到客户端断开；返回前保证所有工作线程已回收。
    void Run();

private:
    bool Handshake();
    void ReaderLoop();
    void CaptureLoop();
    void SenderLoop();

    void Dispatch(const Message& message);
    bool SendNow(Cmd cmd, uint16_t flags, const void* body, std::size_t len);
    void Stop(const char* reason);
    void LogStats(uint64_t window_ms);

    net::Socket socket_;
    std::string peer_;
    const ServerConfig& config_;
    IScreenCapture* capture_;
    IInputInjector* injector_;

    Decoder decoder_;
    BlockingQueue<OutPacket> out_queue_{8};
    FrameEncoder encoder_;
    std::vector<uint8_t> compress_buffer_;

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    bool frame_requested_ = false;
    std::atomic<bool> running_{true};

    // 握手时根据客户端窗口尺寸协商出来的降采样倍数。
    int effective_scale_ = 1;

    std::atomic<uint32_t> tx_seq_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> tiles_sent_{0};
    std::atomic<uint64_t> raw_bytes_sent_{0};
    std::atomic<uint64_t> frames_queued_dropped_{0};
};

}  // namespace rc
