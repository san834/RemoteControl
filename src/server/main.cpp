// 服务端入口：监听 → 接受连接 → 跑一条会话。
#include <cstdio>
#include <string>

#include "capture.h"
#include "inject.h"
#include "rc/cli.h"
#include "rc/dpi.h"
#include "rc/log.h"
#include "rc/net.h"
#include "session.h"

namespace {

rc::net::Listener* g_listener = nullptr;
volatile LONG g_stop = 0;

BOOL WINAPI ConsoleHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_stop, 1);
            if (g_listener) g_listener->Close();
            return TRUE;
        default:
            return FALSE;
    }
}

void PrintUsage() {
    rc::log::Raw(
        "用法: rc_server [选项]\n"
        "  --port <n>        监听端口，默认 9999\n"
        "  --token <s>       连接口令；不填则不校验（局域网演示可用，公网务必设置）\n"
        "  --tile <n>        分块边长，默认 64（16~256）\n"
        "  --scale <n>       降采样倍数 1~4，默认 1（2 = 长宽各减半，像素量 1/4）\n"
        "  --codec <name>    压缩算法：auto（默认）/ huff（快）/ mszip（省带宽）\n"
        "  --max-fps <n>     服务端出帧上限，0 表示不限制\n"
        "  --verbose         每秒打印带宽/帧率/脏块比例\n"
        "  --help            显示本帮助\n");
}

}  // namespace

int main(int argc, char** argv) {
    // 先初始化日志，--help 的输出也要走 UTF-16 控制台通道，否则中文乱码。
    rc::log::Init(nullptr);

    const rc::CliArgs args(argc, argv);
    if (args.Has("help")) {
        PrintUsage();
        return 0;
    }

    rc::EnablePerMonitorDpi();

    rc::net::WsaGuard wsa;
    if (!wsa.ok()) {
        rc::log::Error("WinSock 初始化失败: %s", wsa.error());
        return 1;
    }

    rc::ServerConfig config;
    config.port = static_cast<uint16_t>(args.GetInt("port", 9999));
    config.token_hash = rc::Fnv1a32(args.Get("token"));
    config.tile_size =
        static_cast<uint16_t>(args.GetInt("tile", rc::kDefaultTileSize));
    if (config.tile_size < 16 || config.tile_size > 256) {
        rc::log::Warn("分块边长 %u 超出范围，回退到 %u", config.tile_size,
                      rc::kDefaultTileSize);
        config.tile_size = rc::kDefaultTileSize;
    }
    config.max_fps = args.GetInt("max-fps", 0);
    config.scale = args.GetInt("scale", 1);
    if (config.scale < 1) config.scale = 1;
    if (config.scale > 4) {
        rc::log::Warn("降采样倍数 %d 超出范围，夹到 4", config.scale);
        config.scale = 4;
    }

    const std::string codec = args.Get("codec", "auto");
    if (codec == "huff") {
        config.codec = rc::Compression::kXpressHuff;
    } else if (codec == "mszip") {
        config.codec = rc::Compression::kMsZip;
    } else if (codec != "auto") {
        rc::log::Warn("未知 --codec 取值 '%s'，回退到 auto", codec.c_str());
    }

    config.verbose = args.Has("verbose");

    if (config.token_hash == 0) {
        rc::log::Warn(
            "未设置 --token：任何能访问该端口的主机都可以接管本机，"
            "请仅在可信局域网内使用");
    }

    auto capture = rc::CreateGdiCapture();
    rc::Win32InputInjector injector;

    const rc::Compression effective_codec =
        config.codec != rc::Compression::kNone ? config.codec
                                               : rc::PreferredCompression();
    if (effective_codec == rc::Compression::kNone) {
        rc::log::Warn("系统没有可用的压缩 API，将以未压缩方式传输");
    }
    rc::log::Info("捕获后端: %s / 注入后端: %s / 压缩: %s", capture->name(),
                  injector.name(),
                  effective_codec == rc::Compression::kXpressHuff
                      ? "XPRESS_HUFF"
                      : effective_codec == rc::Compression::kMsZip ? "MSZIP"
                                                                   : "无");

    rc::net::Listener listener;
    std::string error;
    if (!listener.Open(config.port, &error)) {
        rc::log::Error("监听 %u 失败: %s", config.port, error.c_str());
        return 1;
    }
    g_listener = &listener;
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    rc::log::Info("监听 0.0.0.0:%u，等待客户端连接（Ctrl+C 退出）",
                  listener.local_port());

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        std::string peer;
        rc::net::Socket client = listener.Accept(&peer);
        if (!client.valid()) {
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;
            rc::log::Warn("accept 失败: %s", rc::net::LastErrorString().c_str());
            continue;
        }
        rc::log::Info("客户端已连接: %s", peer.c_str());

        rc::ServerSession session(std::move(client), peer, config,
                                  capture.get(), &injector);
        session.Run();

        rc::log::Info("等待下一个客户端…");
    }

    listener.Close();
    g_listener = nullptr;
    rc::log::Info("已退出");
    return 0;
}
