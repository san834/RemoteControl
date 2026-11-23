// WinSock 的最小 RAII 封装。
//
// 相比旧实现的改进：
//   * Socket 拥有资源，析构自动 closesocket，不需要在每个错误分支手写清理；
//   * SendAll 内部加锁并在部分发送时继续发送 —— 旧实现里 UI 线程和收屏线程
//     同时往同一个 socket 写（无锁），TCP 字节流会交错；
//   * 每个调用都有明确的时间上限，不会永久阻塞导致无法退出；
//   * 用 inet_pton 而不是废弃的 inet_addr。
#pragma once

// winsock2.h 必须先于 windows.h 包含，否则会拉进版本冲突的 winsock1 定义。
#include <winsock2.h>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace rc::net {

// WSAStartup / WSACleanup 的 RAII 包装，进程内只需一个实例。
class WsaGuard {
public:
    WsaGuard();
    ~WsaGuard();
    WsaGuard(const WsaGuard&) = delete;
    WsaGuard& operator=(const WsaGuard&) = delete;

    bool ok() const { return ok_; }
    const char* error() const { return error_.c_str(); }

private:
    bool ok_ = false;
    std::string error_;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : s_(s) {}
    ~Socket() { Close(); }

    Socket(Socket&& other) noexcept : s_(other.s_) { other.s_ = INVALID_SOCKET; }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return s_ != INVALID_SOCKET; }
    SOCKET raw() const { return s_; }

    void Close();
    // 半关闭，用于在 Close 之前唤醒对端/本端的阻塞调用。
    void Shutdown();

    // 主动连接。timeout_ms <= 0 表示使用系统默认超时。
    bool Connect(const std::string& host, uint16_t port, int timeout_ms,
                 std::string* error);

    // 线程安全：所有字节要么全部发出，要么返回 false。
    bool SendAll(const void* data, std::size_t len);

    // 返回 >0 读到的字节数；0 表示对端有序关闭；<0 表示出错。
    int RecvSome(void* buf, int len);

    void SetRecvTimeout(int ms);
    void SetSendTimeout(int ms);
    void SetNoDelay(bool on);

private:
    SOCKET s_ = INVALID_SOCKET;
    std::mutex send_mu_;
};

class Listener {
public:
    Listener() = default;
    ~Listener() { Close(); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // port = 0 时由系统分配，实际端口可用 local_port() 查询。
    bool Open(uint16_t port, std::string* error);
    // 阻塞等待一个连接；accept 失败返回无效 Socket。
    Socket Accept(std::string* peer);
    void Close();
    bool valid() const { return s_ != INVALID_SOCKET; }
    uint16_t local_port() const { return port_; }

private:
    SOCKET s_ = INVALID_SOCKET;
    uint16_t port_ = 0;
};

std::string LastErrorString(int code = 0);

}  // namespace rc::net
