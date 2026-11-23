// 客户端入口（GUI 子系统）。
#include <windows.h>

#include <string>

#include "rc/cli.h"
#include "rc/dpi.h"
#include "rc/log.h"
#include "rc/net.h"
#include "window.h"

namespace {

constexpr wchar_t kUsage[] =
    L"用法: rc_client [选项]\n"
    L"  --host <ip/host>   服务端地址，默认 127.0.0.1\n"
    L"  --port <n>         服务端端口，默认 9999\n"
    L"  --token <s>        连接口令（需与服务端一致）\n"
    L"  --show-cursor      在画面上叠加远端光标（默认关闭）\n"
    L"  --log <path>       日志文件，默认 rc_client.log\n"
    L"  --help             显示本帮助\n";

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    const rc::CliArgs args = rc::FromCommandLine();
    if (args.Has("help")) {
        MessageBoxW(nullptr, kUsage, L"rc_client", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    rc::log::Init(args.Get("log", "rc_client.log").c_str());
    rc::EnablePerMonitorDpi();

    rc::net::WsaGuard wsa;
    if (!wsa.ok()) {
        rc::log::Error("WinSock 初始化失败: %s", wsa.error());
        MessageBoxA(nullptr, wsa.error(), "rc_client", MB_OK | MB_ICONERROR);
        return 1;
    }

    rc::ClientConfig config;
    config.host = args.Get("host", "127.0.0.1");
    config.port = static_cast<uint16_t>(args.GetInt("port", 9999));
    config.token_hash = rc::Fnv1a32(args.Get("token"));
    config.verbose = args.Has("verbose");
    config.show_remote_cursor = args.Has("show-cursor");

    rc::log::Info("目标 %s:%u", config.host.c_str(), config.port);

    rc::MainWindow window;
    if (!window.Create(instance, show_command, config)) {
        MessageBoxW(nullptr, L"创建窗口失败，详见日志。", L"rc_client",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    window.RunMessageLoop();
    return 0;
}
