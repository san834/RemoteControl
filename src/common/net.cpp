#include "rc/net.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

namespace rc::net {
namespace {

std::string FormatErrno(int code) {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(code), 0, buf,
                   static_cast<DWORD>(sizeof(buf)), nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

}  // namespace

std::string LastErrorString(int code) {
    const int err = code ? code : WSAGetLastError();
    return FormatErrno(err) + " (WSA " + std::to_string(err) + ")";
}

WsaGuard::WsaGuard() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    ok_ = rc == 0;
    if (!ok_) error_ = LastErrorString(rc);
}

WsaGuard::~WsaGuard() {
    if (ok_) WSACleanup();
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        Close();
        s_ = other.s_;
        other.s_ = INVALID_SOCKET;
    }
    return *this;
}

void Socket::Close() {
    SOCKET s = s_;
    s_ = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
}

void Socket::Shutdown() {
    if (s_ != INVALID_SOCKET) shutdown(s_, SD_BOTH);
}

bool Socket::Connect(const std::string& host, uint16_t port, int timeout_ms,
                     std::string* error) {
    Close();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 ||
        !result) {
        if (error) *error = "无法解析主机 " + host;
        return false;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        if (error) *error = LastErrorString();
        return false;
    }

    if (timeout_ms <= 0) {
        const int rc = ::connect(s, result->ai_addr,
                                 static_cast<int>(result->ai_addrlen));
        freeaddrinfo(result);
        if (rc == SOCKET_ERROR) {
            if (error) *error = LastErrorString();
            closesocket(s);
            return false;
        }
        s_ = s;
        SetNoDelay(true);
        return true;
    }

    // 带超时的 connect：切成非阻塞 → select 等待可写 → 检查 SO_ERROR。
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    const int rc = ::connect(s, result->ai_addr,
                             static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);

    bool connected = false;
    if (rc == 0) {
        connected = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s, &write_set);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, nullptr, &write_set, nullptr, &tv) == 1) {
            int so_error = 0;
            int len = sizeof(so_error);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&so_error), &len) == 0 &&
                so_error == 0) {
                connected = true;
            } else if (error) {
                *error = LastErrorString(so_error);
            }
        } else if (error) {
            *error = "连接超时";
        }
    } else if (error) {
        *error = LastErrorString();
    }

    if (!connected) {
        closesocket(s);
        return false;
    }

    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    s_ = s;
    SetNoDelay(true);
    return true;
}

bool Socket::SendAll(const void* data, std::size_t len) {
    std::lock_guard<std::mutex> lock(send_mu_);
    if (s_ == INVALID_SOCKET) return false;
    const auto* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s_, p + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

int Socket::RecvSome(void* buf, int len) {
    if (s_ == INVALID_SOCKET) return -1;
    // MSG_WAITALL 会让"半包"变成"整包"，这里只需要拿到现有字节，不用它。
    return ::recv(s_, static_cast<char*>(buf), len, 0);
}

void Socket::SetRecvTimeout(int ms) {
    if (s_ == INVALID_SOCKET) return;
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
}

void Socket::SetSendTimeout(int ms) {
    if (s_ == INVALID_SOCKET) return;
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
}

void Socket::SetNoDelay(bool on) {
    if (s_ == INVALID_SOCKET) return;
    BOOL v = on ? TRUE : FALSE;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&v), sizeof(v));
}

bool Listener::Open(uint16_t port, std::string* error) {
    Close();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (error) *error = LastErrorString();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        if (error) *error = LastErrorString();
        closesocket(s);
        return false;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        if (error) *error = LastErrorString();
        closesocket(s);
        return false;
    }

    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }
    s_ = s;
    return true;
}

Socket Listener::Accept(std::string* peer) {
    if (s_ == INVALID_SOCKET) return Socket();
    sockaddr_in addr{};
    int len = sizeof(addr);
    SOCKET c = ::accept(s_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (c == INVALID_SOCKET) return Socket();
    if (peer) {
        char ip[64] = {};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        *peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    Socket sock(c);
    sock.SetNoDelay(true);
    return sock;
}

void Listener::Close() {
    SOCKET s = s_;
    s_ = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
}

}  // namespace rc::net
