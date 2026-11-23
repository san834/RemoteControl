// 客户端会话：连接管理 + 收帧线程 + 发送线程。
//
// 线程与所有权：
//   收帧线程（reader）  持有 socket 的 shared_ptr，负责 connect/握手/recv/解码
//   发送线程（writer）  唯一的 socket 写入者，负责所有请求、输入事件、心跳
//   UI 线程             只在 paint_mutex_ 保护下读表面，从不直接碰 socket
//
// 旧实现里 UI 线程和收屏线程各自 send 同一个 socket 且完全无锁，这是会导致
// 偶发花屏/卡死的真实数据竞争。shared_ptr<Socket> 的持有方式同时解决了
// "线程 A 正在 send、线程 B 已经把 socket 关掉"的 use-after-close 问题。
#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rc/frame.h"
#include "rc/net.h"
#include "rc/protocol.h"
#include "rc/queue.h"
#include "rc/stream.h"
#include "surface.h"

namespace rc {

// 收帧线程 → UI 线程的窗口消息。
inline constexpr UINT kMsgFrameReady = WM_APP + 1;
inline constexpr UINT kMsgStateChanged = WM_APP + 2;

struct ClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9999;
    uint32_t token_hash = 0;
    int connect_timeout_ms = 3000;
    int idle_timeout_ms = 8000;
    bool verbose = false;
    // 默认不在画面上叠加远端光标：本机鼠标指针就在那儿，再画一个只会让人
    // 看到"两个指针"，而且叠加的那个要等下一帧才更新，看起来就是"鼠标延迟"。
    bool show_remote_cursor = false;
};

enum class ConnState { kDisconnected, kConnecting, kConnected };

class ClientSession {
public:
    ClientSession(HWND notify_window, const ClientConfig& config);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void Start();
    void Stop();

    // --- 以下均在 UI 线程调用 ---

    // 把最新一帧等比绘制到 dc（客户区坐标），并叠加远端光标。
    // 没有可用画面时返回 false，由调用方画背景。
    bool Render(HDC dc, const RECT& client);

    // 客户区坐标 → 远端虚拟桌面绝对坐标。越界返回 false。
    bool MapClientToRemote(int client_x, int client_y, const RECT& client,
                           int* remote_x, int* remote_y) const;

    bool SendMouse(const MouseBody& mouse);
    bool SendKey(const KeyBody& key);
    void RequestFrame();

    ConnState state() const;
    std::string status() const;
    int remote_width() const { return remote_width_.load(); }
    int remote_height() const { return remote_height_.load(); }

private:
    void ReaderLoop();
    void WriterLoop();
    std::shared_ptr<net::Socket> ConnectAndHandshake(std::string* error);
    void HandleMessage(const Message& message);
    void HandleFrame(const Message& message);
    bool Enqueue(std::vector<uint8_t> bytes);
    void SetState(ConnState state, std::string text);
    void MarkDisconnected(const std::string& reason);
    uint32_t NextSeq() { return ++tx_seq_; }

    HWND hwnd_;
    ClientConfig config_;

    std::mutex conn_mutex_;
    std::shared_ptr<net::Socket> socket_;

    mutable std::mutex state_mutex_;
    ConnState state_ = ConnState::kDisconnected;
    std::string status_;

    mutable std::mutex paint_mutex_;
    Surface surface_;
    POINT cursor_{0, 0};
    bool cursor_visible_ = false;

    // 以下几项被 UI 线程在"没有画面锁"的情况下读取（鼠标事件映射路径），
    // 因此单独用原子量发布。鼠标处理绝不能去抢 paint_mutex_：收帧线程在
    // 解码整帧时会长时间持有它，鼠标一等等就是几十毫秒，手感直接崩掉。
    std::atomic<bool> has_frame_{false};
    std::atomic<int> surface_width_{0};
    std::atomic<int> surface_height_{0};

    BlockingQueue<std::vector<uint8_t>> out_queue_{64};
    Decoder decoder_;
    std::thread reader_;
    std::thread writer_;
    std::atomic<bool> running_{true};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> tx_seq_{0};
    std::atomic<uint64_t> last_rx_ms_{0};
    std::atomic<int> remote_width_{0};
    std::atomic<int> remote_height_{0};
    std::atomic<int> origin_x_{0};
    std::atomic<int> origin_y_{0};
    std::atomic<int> scale_{1};

    // 请求-响应锁步：同一时刻最多一条帧请求在途，避免 UI 重绘风暴把服务端
    // 打成一帧接一帧地满负荷编码。
    std::atomic<bool> frame_pending_{false};
    std::atomic<uint64_t> last_request_ms_{0};
    bool show_cursor_ = false;

    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> bytes_{0};
};

}  // namespace rc
