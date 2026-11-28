#include "session.h"

#include <algorithm>
#include <chrono>

#include "rc/compress.h"
#include "rc/log.h"

namespace rc {
namespace {

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace
ServerSession::ServerSession(net::Socket socket, std::string peer,
                             const ServerConfig& config, IScreenCapture* capture,
                             IInputInjector* injector)
    : socket_(std::move(socket)),
      peer_(std::move(peer)),
      config_(config),
      capture_(capture),
      injector_(injector),
      encoder_(config.tile_size) {}

ServerSession::~ServerSession() {
    running_ = false;
    frame_cv_.notify_all();
    out_queue_.Close();
}

bool ServerSession::SendNow(Cmd cmd, uint16_t flags, const void* body,
                            std::size_t len) {
    const uint32_t seq = ++tx_seq_;
    const std::vector<uint8_t> packet = Encode(cmd, seq, flags, body, len);
    if (!socket_.SendAll(packet.data(), packet.size())) return false;
    bytes_sent_ += packet.size();
    ++packets_sent_;
    return true;
}

bool ServerSession::Handshake() {
    socket_.SetRecvTimeout(2000);
    const uint64_t deadline = NowMs() + 5000;
    Message message;
    bool got_message = false;
    std::vector<uint8_t> buffer(16 * 1024);

    while (NowMs() < deadline) {
        if (decoder_.Pop(message)) {
            got_message = true;
            break;
        }
        const int n = socket_.RecvSome(buffer.data(),
                                       static_cast<int>(buffer.size()));
        if (n > 0) {
            decoder_.Push(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            log::Warn("[%s] 握手阶段对端关闭连接", peer_.c_str());
            return false;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) continue;
        log::Warn("[%s] 握手失败: %s", peer_.c_str(),
                  net::LastErrorString(err).c_str());
        return false;
    }

    // 循环是因为超时退出的（一个完整包都没收到）。这里必须单独判定：
    // Message 的 cmd 有默认值 kHello，直接落到下面的命令检查会把"超时"
    // 误报成"版本不匹配"，还会给一个没说过话的客户端发 Bye。
    if (!got_message) {
        log::Warn("[%s] 握手超时：5 秒内没有收到完整的 Hello", peer_.c_str());
        return false;
    }

    if (message.cmd != Cmd::kHello) {
        log::Warn("[%s] 期望 Hello，实际收到 cmd=%u", peer_.c_str(),
                  static_cast<unsigned>(message.cmd));
        return false;
    }

    HelloBody hello{};
    if (!message.As(hello) || hello.version != kVersion) {
        ByeBody bye{};
        bye.reason = kByeVersionMismatch;
        SendNow(Cmd::kBye, 0, &bye, sizeof(bye));
        log::Warn("[%s] 协议版本不匹配（对端 v%u，本端 v%u）", peer_.c_str(),
                  hello.version, kVersion);
        return false;
    }
    if (config_.token_hash != 0 && hello.token_hash != config_.token_hash) {
        ByeBody bye{};
        bye.reason = kByeAuthFailed;
        SendNow(Cmd::kBye, 0, &bye, sizeof(bye));
        log::Warn("[%s] 口令校验失败", peer_.c_str());
        return false;
    }

    // 用一次完整分辨率的捕获换取几何信息，顺带预热 DIB 表面。
    CapturedFrame frame;
    char error[128] = {};
    if (!capture_->Capture(frame, 1, error, sizeof(error))) {
        log::Error("[%s] 握手期间捕获屏幕失败: %s", peer_.c_str(), error);
        return false;
    }

    // 协商降采样：客户端窗口比远端桌面小多少，就降多少。
    int requested = 1;
    if (hello.client_width > 0 && hello.client_height > 0) {
        const int by_width = frame.width / hello.client_width;
        const int by_height = frame.height / hello.client_height;
        requested = std::min(by_width, by_height);
    }
    effective_scale_ =
        std::clamp(std::max(config_.scale, requested), 1, 4);

    HelloAckBody ack{};
    ack.version = kVersion;
    ack.tile_size = config_.tile_size;
    ack.origin_x = frame.origin_x;
    ack.origin_y = frame.origin_y;
    ack.screen_w = std::max(1, frame.width / effective_scale_);
    ack.screen_h = std::max(1, frame.height / effective_scale_);
    ack.scale = static_cast<uint16_t>(effective_scale_);
    if (!SendNow(Cmd::kHelloAck, 0, &ack, sizeof(ack))) return false;

    log::Info(
        "[%s] 握手完成：桌面 %dx%d，降采样 1/%d → %dx%d，原点 (%d,%d)，分块 %u",
        peer_.c_str(), frame.width, frame.height, effective_scale_,
        ack.screen_w, ack.screen_h, frame.origin_x, frame.origin_y,
        config_.tile_size);
    return true;
}

void ServerSession::Run() {
    if (!Handshake()) {
        Stop("握手失败");
        return;
    }

    // 丢弃握手期残留数据，进入稳态。
    decoder_.Reset();
    socket_.SetRecvTimeout(500);

    std::thread capturer([this] { CaptureLoop(); });
    std::thread sender([this] { SenderLoop(); });

    ReaderLoop();

    Stop("会话结束");
    capturer.join();
    sender.join();
}

void ServerSession::Stop(const char* reason) {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    log::Info("[%s] 断开：%s", peer_.c_str(), reason);
    frame_cv_.notify_all();
    out_queue_.Close();
    socket_.Shutdown();
}

void ServerSession::Dispatch(const Message& message) {
    switch (message.cmd) {
        case Cmd::kFrameRequest: {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            frame_requested_ = true;
            frame_cv_.notify_one();
            break;
        }
        case Cmd::kMouse: {
            MouseBody mouse{};
            if (message.As(mouse)) injector_->Mouse(mouse);
            break;
        }
        case Cmd::kKey: {
            KeyBody key{};
            if (message.As(key)) injector_->Key(key);
            break;
        }
        case Cmd::kPing:
            SendNow(Cmd::kPong, 0, nullptr, 0);
            break;
        case Cmd::kBye:
            Stop("客户端主动断开");
            break;
        default:
            log::Warn("[%s] 忽略未知命令 %u", peer_.c_str(),
                      static_cast<unsigned>(message.cmd));
            break;
    }
}

void ServerSession::ReaderLoop() {
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t last_rx = NowMs();

    while (running_) {
        const int n = socket_.RecvSome(buffer.data(),
                                       static_cast<int>(buffer.size()));
        if (n > 0) {
            last_rx = NowMs();
            decoder_.Push(buffer.data(), static_cast<std::size_t>(n));
            Message message;
            while (decoder_.Pop(message)) {
                Dispatch(message);
                if (!running_) break;
            }
            continue;
        }
        if (n == 0) {
            Stop("对端关闭连接");
            break;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            if (NowMs() - last_rx >
                static_cast<uint64_t>(config_.idle_timeout_ms)) {
                Stop("心跳超时");
                break;
            }
            continue;
        }
        Stop(net::LastErrorString(err).c_str());
        break;
    }
}

void ServerSession::CaptureLoop() {
    const uint64_t min_interval_ms =
        config_.max_fps > 0 ? 1000u / static_cast<uint64_t>(config_.max_fps) : 0;
    uint64_t last_frame_ms = 0;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            frame_cv_.wait(lock, [this] {
                return frame_requested_ || !running_;
            });
            if (!running_) break;
            frame_requested_ = false;
        }

        if (min_interval_ms) {
            const uint64_t now = NowMs();
            const uint64_t elapsed = now - last_frame_ms;
            if (last_frame_ms && elapsed < min_interval_ms) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(min_interval_ms - elapsed));
            }
        }

        CapturedFrame captured;
        char error[128] = {};
        if (!capture_->Capture(captured, effective_scale_, error, sizeof(error))) {
            log::Error("[%s] 捕获失败: %s", peer_.c_str(), error);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        FrameImage image;
        image.width = captured.width;
        image.height = captured.height;
        image.pixels = captured.pixels.data();
        image.cursor_x = captured.cursor_x;
        image.cursor_y = captured.cursor_y;
        image.cursor_visible = captured.cursor_visible;

        EncodedFrame encoded = encoder_.Encode(image);
        if (encoded.body.empty()) continue;

        OutPacket packet;
        packet.is_frame = true;
        packet.tiles = encoded.tiles_dirty;
        packet.raw_bytes = encoded.raw_bytes;
        packet.bytes = std::move(encoded.body);

        uint16_t flags = 0;
        if (encoded.key_frame) flags |= kFlagKeyFrame;
        if (static_cast<int>(packet.bytes.size()) >= config_.min_compress_bytes) {
            const Compression algorithm = config_.codec != Compression::kNone
                                           ? config_.codec
                                           : PreferredCompression();
            if (algorithm != Compression::kNone &&
                CompressData(packet.bytes.data(), packet.bytes.size(),
                             algorithm, compress_buffer_) &&
                compress_buffer_.size() + 32 < packet.bytes.size()) {
                packet.bytes.swap(compress_buffer_);
                compress_buffer_.clear();
                flags |= algorithm == Compression::kXpressHuff
                             ? kFlagCompressedHuff
                             : kFlagCompressed;
            }
        }

        // 把 flags 写进包头：Encode 需要知道最终 flags，因此在这里成包。
        const std::vector<uint8_t> wire =
            Encode(Cmd::kFrame, ++tx_seq_, flags, packet.bytes.data(),
                   packet.bytes.size());
        packet.bytes = wire;

        last_frame_ms = NowMs();
        if (!out_queue_.Push(std::move(packet))) {
            ++frames_queued_dropped_;
        }
    }
}

void ServerSession::SenderLoop() {
    uint64_t last_stats = NowMs();
    OutPacket packet;

    while (out_queue_.Pop(packet)) {
        if (!socket_.SendAll(packet.bytes.data(), packet.bytes.size())) {
            Stop("发送失败");
            break;
        }
        bytes_sent_ += packet.bytes.size();
        ++packets_sent_;
        if (packet.is_frame) {
            ++frames_sent_;
            tiles_sent_ += static_cast<uint64_t>(packet.tiles);
            raw_bytes_sent_ += packet.raw_bytes;
        }

        const uint64_t now = NowMs();
        if (now - last_stats >= 1000) {
            LogStats(now - last_stats);
            last_stats = now;
        }
    }
}

void ServerSession::LogStats(uint64_t window_ms) {
    if (config_.verbose) {
        const uint64_t bytes = bytes_sent_.exchange(0);
        const uint64_t frames = frames_sent_.exchange(0);
        const uint64_t tiles = tiles_sent_.exchange(0);
        const uint64_t raw = raw_bytes_sent_.exchange(0);
        const double seconds = static_cast<double>(window_ms) / 1000.0;
        log::Info(
            "[%s] %.1f fps | 出站 %.1f KB/s | 脏块 %.1f/帧 | 像素搬运 %.1f KB/s",
            peer_.c_str(), frames / seconds,
            bytes / 1024.0 / seconds,
            frames ? static_cast<double>(tiles) / static_cast<double>(frames) : 0.0,
            raw / 1024.0 / seconds);
    }
}

}  // namespace rc
