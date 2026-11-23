#include "session.h"

#include <algorithm>
#include <chrono>

#include "rc/clock.h"
#include "rc/compress.h"
#include "rc/log.h"

namespace rc {
namespace {

std::vector<uint8_t> EmptyBodyPacket(Cmd cmd, uint32_t seq) {
    return Encode(cmd, seq, 0, nullptr, 0);
}

HCURSOR ArrowCursor() {
    static HCURSOR cached = LoadCursorA(nullptr, IDC_ARROW);
    return cached;
}

}  // namespace

ClientSession::ClientSession(HWND notify_window, const ClientConfig& config)
    : hwnd_(notify_window),
      config_(config),
      decoder_(),
      show_cursor_(config.show_remote_cursor) {
    SetState(ConnState::kDisconnected, "未连接");
}

ClientSession::~ClientSession() { Stop(); }

void ClientSession::Start() {
    reader_ = std::thread([this] { ReaderLoop(); });
    writer_ = std::thread([this] { WriterLoop(); });
}

void ClientSession::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    out_queue_.Close();
    MarkDisconnected("正在退出");
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
}

void ClientSession::SetState(ConnState state, std::string text) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = state;
        status_ = std::move(text);
    }
    if (hwnd_) PostMessageW(hwnd_, kMsgStateChanged, 0, 0);
}

ConnState ClientSession::state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

std::string ClientSession::status() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_;
}

void ClientSession::MarkDisconnected(const std::string& reason) {
    std::shared_ptr<net::Socket> sock;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        sock = socket_;
        socket_.reset();
    }
    connected_ = false;
    frame_pending_ = false;
    // Shutdown 会唤醒 still-blocked 的 recv，让收帧线程尽快回到重连分支。
    if (sock) sock->Shutdown();
    if (running_) SetState(ConnState::kDisconnected, reason);
}

bool ClientSession::Enqueue(std::vector<uint8_t> bytes) {
    return out_queue_.Push(std::move(bytes));
}

void ClientSession::RequestFrame() {
    if (!connected_) return;

    const uint64_t now = NowMs();
    bool expected = false;
    if (!frame_pending_.compare_exchange_strong(expected, true)) {
        // 已有请求在途。正常情况直接返回即可（锁步）；但万一对端把请求丢了，
        // 超过 1 秒就允许补发一次，避免画面永久卡死。
        if (now - last_request_ms_.load() < 1000) return;
    }
    last_request_ms_ = now;
    Enqueue(EmptyBodyPacket(Cmd::kFrameRequest, NextSeq()));
}

bool ClientSession::SendMouse(const MouseBody& mouse) {
    if (!connected_) return false;
    return Enqueue(Encode(Cmd::kMouse, NextSeq(), 0, &mouse, sizeof(mouse)));
}

bool ClientSession::SendKey(const KeyBody& key) {
    if (!connected_) return false;
    return Enqueue(Encode(Cmd::kKey, NextSeq(), 0, &key, sizeof(key)));
}

std::shared_ptr<net::Socket> ClientSession::ConnectAndHandshake(
    std::string* error) {
    auto sock = std::make_shared<net::Socket>();
    std::string detail;
    if (!sock->Connect(config_.host, config_.port, config_.connect_timeout_ms,
                       &detail)) {
        if (error) *error = "连接失败：" + detail;
        return nullptr;
    }
    sock->SetRecvTimeout(2000);

    HelloBody hello{};
    hello.version = kVersion;
    hello.token_hash = config_.token_hash;
    // 告诉服务端"我这边能显示多大"，由服务端决定降采样倍数。
    if (hwnd_) {
        RECT client{};
        if (GetClientRect(hwnd_, &client)) {
            hello.client_width =
                static_cast<uint16_t>(std::clamp<int>(client.right - client.left,
                                                      1, 65535));
            hello.client_height =
                static_cast<uint16_t>(std::clamp<int>(client.bottom - client.top,
                                                      1, 65535));
        }
    }
    const std::vector<uint8_t> packet =
        Encode(Cmd::kHello, NextSeq(), 0, &hello, sizeof(hello));
    if (!sock->SendAll(packet.data(), packet.size())) {
        if (error) *error = "握手发送失败";
        return nullptr;
    }

    Decoder handshake;
    std::vector<uint8_t> buffer(16 * 1024);
    const uint64_t deadline = NowMs() + 5000;
    Message message;
    bool got_reply = false;

    while (NowMs() < deadline) {
        if (handshake.Pop(message)) {
            got_reply = true;
            break;
        }
        const int n = sock->RecvSome(buffer.data(),
                                     static_cast<int>(buffer.size()));
        if (n > 0) {
            handshake.Push(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            if (error) *error = "握手中对端关闭连接";
            return nullptr;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) continue;
        if (error) *error = "握手失败：" + net::LastErrorString(err);
        return nullptr;
    }

    if (!got_reply) {
        if (error) *error = "握手超时";
        return nullptr;
    }

    if (message.cmd == Cmd::kBye) {
        ByeBody bye{};
        message.As(bye);
        if (error) {
            *error = bye.reason == kByeAuthFailed ? "服务端拒绝：口令错误"
                     : bye.reason == kByeVersionMismatch
                         ? "服务端拒绝：协议版本不匹配"
                         : "服务端主动断开";
        }
        return nullptr;
    }
    if (message.cmd != Cmd::kHelloAck) {
        if (error) *error = "握手应答类型错误";
        return nullptr;
    }

    HelloAckBody ack{};
    if (!message.As(ack) || ack.version != kVersion) {
        if (error) *error = "握手应答无法解析";
        return nullptr;
    }

    remote_width_ = ack.screen_w;
    remote_height_ = ack.screen_h;
    origin_x_ = ack.origin_x;
    origin_y_ = ack.origin_y;
    scale_ = ack.scale > 0 ? ack.scale : 1;

    sock->SetRecvTimeout(500);
    return sock;
}

void ClientSession::ReaderLoop() {
    std::vector<uint8_t> buffer(64 * 1024);

    while (running_) {
        std::shared_ptr<net::Socket> sock;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            sock = socket_;
        }

        if (!sock) {
            SetState(ConnState::kConnecting,
                     "正在连接 " + config_.host + ":" +
                         std::to_string(config_.port));
            std::string error;
            sock = ConnectAndHandshake(&error);
            if (!sock) {
                SetState(ConnState::kDisconnected, error);
                log::Warn("%s", error.c_str());
                // 退避，避免连不上时把 CPU 打满。
                for (int i = 0; i < 10 && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(conn_mutex_);
                socket_ = sock;
            }
            decoder_.Reset();
            out_queue_.Clear();
            frames_ = 0;
            bytes_ = 0;
            last_rx_ms_ = NowMs();
            frame_pending_ = false;
            connected_ = true;
            SetState(ConnState::kConnected, "已连接");
            log::Info("已连接 %s:%u，远端 %dx%d", config_.host.c_str(),
                      config_.port, remote_width_.load(),
                      remote_height_.load());
            RequestFrame();
            continue;
        }

        const int n =
            sock->RecvSome(buffer.data(), static_cast<int>(buffer.size()));
        if (n > 0) {
            last_rx_ms_ = NowMs();
            bytes_ += static_cast<uint64_t>(n);
            decoder_.Push(buffer.data(), static_cast<std::size_t>(n));
            Message message;
            while (decoder_.Pop(message)) {
                HandleMessage(message);
                if (!running_) break;
            }
            continue;
        }
        if (n == 0) {
            MarkDisconnected("服务端关闭连接");
            continue;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            if (NowMs() - last_rx_ms_.load() >
                static_cast<uint64_t>(config_.idle_timeout_ms)) {
                MarkDisconnected("心跳超时");
            }
            continue;
        }
        MarkDisconnected("接收失败：" + net::LastErrorString(err));
    }
}

void ClientSession::WriterLoop() {
    uint64_t last_ping = NowMs();
    std::vector<uint8_t> packet;

    while (running_) {
        if (out_queue_.PopFor(packet, std::chrono::milliseconds(500))) {
            std::shared_ptr<net::Socket> sock;
            {
                std::lock_guard<std::mutex> lock(conn_mutex_);
                sock = socket_;
            }
            if (sock && !sock->SendAll(packet.data(), packet.size())) {
                MarkDisconnected("发送失败");
            }
        }
        if (!running_) break;

        // 心跳：即使 UI 静止也要有流量，否则两端的"静默超时"无法区分
        // "用户没操作"和"链路已断"。
        const uint64_t now = NowMs();
        if (now - last_ping >= 2000) {
            last_ping = now;
            if (connected_) {
                Enqueue(EmptyBodyPacket(Cmd::kPing, NextSeq()));
            }
        }
    }
}

void ClientSession::HandleMessage(const Message& message) {
    switch (message.cmd) {
        case Cmd::kFrame:
            HandleFrame(message);
            break;
        case Cmd::kPong:
        case Cmd::kHelloAck:
            break;
        case Cmd::kBye:
            MarkDisconnected("服务端断开连接");
            break;
        default:
            log::Debug("忽略未知命令 %u",
                       static_cast<unsigned>(message.cmd));
            break;
    }
}

void ClientSession::HandleFrame(const Message& message) {
    std::vector<uint8_t> inflated;
    const uint8_t* body = message.body.data();
    std::size_t len = message.body.size();

    Compression algorithm = Compression::kNone;
    if (message.flags & kFlagCompressedHuff) {
        algorithm = Compression::kXpressHuff;
    } else if (message.flags & kFlagCompressed) {
        algorithm = Compression::kMsZip;
    }
    if (algorithm != Compression::kNone) {
        if (!DecompressData(body, len, algorithm, kMaxBodyBytes, inflated)) {
            log::Warn("帧解压失败（%zu 字节）", len);
            return;
        }
        body = inflated.data();
        len = inflated.size();
    }

    FrameDecoder decoder;
    if (!decoder.Begin(body, len)) {
        log::Warn("帧体校验失败");
        return;
    }
    const FrameHeadView& head = decoder.head();

    {
        std::lock_guard<std::mutex> lock(paint_mutex_);
        if (head.screen_w != surface_.width() ||
            head.screen_h != surface_.height()) {
            // 先撤掉"可绘制"标志，再换表面：否则 UI 线程可能拿着旧尺寸去映射坐标。
            has_frame_ = false;
            if (!surface_.Resize(head.screen_w, head.screen_h)) {
                log::Error("创建渲染表面失败（%dx%d）", head.screen_w,
                           head.screen_h);
                return;
            }
            surface_width_ = head.screen_w;
            surface_height_ = head.screen_h;
        }
        FrameDecoder::Target target{surface_.bits(), surface_.stride()};
        if (!decoder.BlitTo(target)) {
            log::Warn("脏块写入失败");
            return;
        }
        cursor_.x = head.cursor_x;
        cursor_.y = head.cursor_y;
        cursor_visible_ = head.cursor_visible;
        has_frame_ = true;
    }

    ++frames_;
    frame_pending_ = false;  // 这一帧到了，可以请求下一帧
    if (hwnd_) PostMessageW(hwnd_, kMsgFrameReady, 0, 0);
}

bool ClientSession::Render(HDC dc, const RECT& client) {
    std::lock_guard<std::mutex> lock(paint_mutex_);
    if (!has_frame_ || !surface_.valid()) return false;

    const RECT dest =
        FitRect(client.right - client.left, client.bottom - client.top,
                surface_.width(), surface_.height());
    const int dest_w = dest.right - dest.left;
    const int dest_h = dest.bottom - dest.top;
    if (dest_w <= 0 || dest_h <= 0) return true;

    if (!surface_.Blit(dc, dest.left, dest.top, dest_w, dest_h)) return false;

    // 默认不叠加远端光标：本机指针已经在那儿了，再画一个会出现"两个指针"，
    // 而且叠加的那个要等下一帧才移动，看上去就是"鼠标延迟"。
    if (show_cursor_ && cursor_visible_) {
        const double sx = static_cast<double>(dest_w) / surface_.width();
        const double sy = static_cast<double>(dest_h) / surface_.height();
        const int cx = dest.left + static_cast<int>(cursor_.x * sx);
        const int cy = dest.top + static_cast<int>(cursor_.y * sy);
        DrawIconEx(dc, cx, cy, ArrowCursor(), 0, 0, 0, nullptr, DI_NORMAL);
    }
    return true;
}

bool ClientSession::MapClientToRemote(int client_x, int client_y,
                                      const RECT& client, int* remote_x,
                                      int* remote_y) const {
    // 刻意不加 paint_mutex_：这个函数在 UI 线程的每一条鼠标消息里都会被调用，
    // 而收帧线程解码整帧时会长时间持有该锁。抢锁的代价就是鼠标发涩。
    // 需要的几何信息全部由原子量发布，读到的是某一时刻的一致快照。
    if (!has_frame_.load()) return false;

    const int surface_w = surface_width_.load();
    const int surface_h = surface_height_.load();
    if (surface_w <= 0 || surface_h <= 0) return false;

    const RECT dest = FitRect(client.right - client.left,
                              client.bottom - client.top, surface_w, surface_h);
    const int dest_w = dest.right - dest.left;
    const int dest_h = dest.bottom - dest.top;
    if (dest_w <= 0 || dest_h <= 0) return false;

    // 点在黑边上时按最近的边处理，避免"点不中"的体感问题。
    const int cx = std::clamp(client_x, static_cast<int>(dest.left),
                              static_cast<int>(dest.right) - 1);
    const int cy = std::clamp(client_y, static_cast<int>(dest.top),
                              static_cast<int>(dest.bottom) - 1);

    int rx = static_cast<int>(static_cast<long long>(cx - dest.left) *
                              surface_w / dest_w);
    int ry = static_cast<int>(static_cast<long long>(cy - dest.top) *
                              surface_h / dest_h);
    rx = std::clamp(rx, 0, surface_w - 1);
    ry = std::clamp(ry, 0, surface_h - 1);

    const int scale = scale_.load() > 0 ? scale_.load() : 1;
    if (remote_x) *remote_x = origin_x_.load() + rx * scale;
    if (remote_y) *remote_y = origin_y_.load() + ry * scale;
    return true;
}

}  // namespace rc
