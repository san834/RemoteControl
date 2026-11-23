#include "window.h"

#include <windowsx.h>

#include <cstdio>

#include "rc/clock.h"
#include "rc/dpi.h"
#include "rc/log.h"

namespace rc {
namespace {

constexpr wchar_t kWindowClass[] = L"RemoteControlClientView";
constexpr UINT kMinMoveIntervalMs = 16;  // ~60Hz 上限，避免鼠标事件淹没链路

}  // namespace

MainWindow::~MainWindow() {
    if (session_) session_->Stop();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool MainWindow::Create(HINSTANCE instance, int show_command,
                        const ClientConfig& config) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    // CS_DBLCLKS 必不可少：没有它 Windows 不会投递 WM_*BUTTONDBLCLK，
    // 双击处理分支就永远是死代码。
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        log::Error("注册窗口类失败: %lu", GetLastError());
        return false;
    }

    title_ = L"RemoteControl — " + std::wstring(config.host.begin(),
                                                config.host.end());
    hwnd_ = CreateWindowExW(0, kWindowClass, title_.c_str(),
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            1024, 640, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        log::Error("创建窗口失败: %lu", GetLastError());
        return false;
    }

    session_ = std::make_unique<ClientSession>(hwnd_, config);

    ShowWindow(hwnd_, show_command);
    UpdateWindow(hwnd_);
    session_->Start();
    return true;
}

void MainWindow::RunMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self =
        reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->WndProc(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT MainWindow::WndProc(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            // 全窗口都由 WM_PAINT 覆盖，屏蔽擦除以消除闪烁。
            return 1;

        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case kMsgFrameReady:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case kMsgStateChanged:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE: {
            const ULONGLONG now = GetTickCount64();
            if (now - last_move_tick_ < kMinMoveIntervalMs) return 0;
            last_move_tick_ = now;
            SendMouse(MouseAction::kMove, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            return 0;
        }

        case WM_LBUTTONDOWN:
            // 捕获鼠标：拖拽时指针移出窗口也要继续收到事件。
            SetCapture(hwnd_);
            SendMouse(MouseAction::kLeftDown, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            return 0;

        case WM_LBUTTONUP:
            SendMouse(MouseAction::kLeftUp, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
            SetCapture(hwnd_);
            SendMouse(MouseAction::kRightDown, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            return 0;

        case WM_RBUTTONUP:
            SendMouse(MouseAction::kRightUp, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            ReleaseCapture();
            return 0;

        case WM_MBUTTONDOWN:
            SetCapture(hwnd_);
            SendMouse(MouseAction::kMiddleDown, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            return 0;

        case WM_MBUTTONUP:
            SendMouse(MouseAction::kMiddleUp, GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam));
            ReleaseCapture();
            return 0;

        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
            // 双击不再需要单独的协议命令：连续两次 down/up 就是双击，
            // 远端系统的判定和本地完全一致。
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            SendMouse(MouseAction::kWheel, pt.x, pt.y,
                      GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            SendKey(message, wparam, lparam);
            return 0;

        case WM_KILLFOCUS:
            // 失焦时补发所有按键的抬起，避免远端"卡键"。
            for (int vk = 0; vk < 256; ++vk) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    KeyBody key{};
                    key.vk = static_cast<uint16_t>(vk);
                    key.state = static_cast<uint8_t>(KeyState::kUp);
                    if (session_) session_->SendKey(key);
                }
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

void MainWindow::SendMouse(MouseAction action, int client_x, int client_y,
                           int wheel_units) {
    if (!session_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    int remote_x = 0;
    int remote_y = 0;
    if (!session_->MapClientToRemote(client_x, client_y, client, &remote_x,
                                     &remote_y)) {
        return;  // 还没有画面，丢弃事件
    }
    MouseBody mouse{};
    mouse.action = static_cast<uint8_t>(action);
    mouse.wheel = static_cast<int16_t>(wheel_units);
    mouse.x = remote_x;
    mouse.y = remote_y;
    session_->SendMouse(mouse);
}

void MainWindow::SendKey(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!session_) return;
    KeyBody key{};
    key.vk = static_cast<uint16_t>(wparam & 0xFFu);
    key.scan = static_cast<uint16_t>((lparam >> 16) & 0xFFu);
    key.state = static_cast<uint8_t>(
        (message == WM_KEYUP || message == WM_SYSKEYUP) ? KeyState::kUp
                                                        : KeyState::kDown);
    session_->SendKey(key);
}

void MainWindow::DrawStatus(HDC dc, const RECT& client) {
    const std::string status =
        session_ ? session_->status() : std::string("未初始化");
    const int width = session_ ? session_->remote_width() : 0;
    const int height = session_ ? session_->remote_height() : 0;

    wchar_t text[512];
    if (width > 0 && height > 0) {
        std::swprintf(text, 512, L"%hs\r\n远端分辨率 %d × %d", status.c_str(),
                      width, height);
    } else {
        std::swprintf(text, 512, L"%hs", status.c_str());
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(210, 210, 210));
    DrawTextW(dc, text, -1, const_cast<RECT*>(&client),
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    RECT client{};
    GetClientRect(hwnd_, &client);

    bool painted = false;
    if (session_) painted = session_->Render(dc, client);
    if (!painted) {
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        DrawStatus(dc, client);
    }

    EndPaint(hwnd_, &ps);

    // 画完立刻请求下一帧：请求-响应锁步，天然形成背压，不会堆积帧。
    if (session_) session_->RequestFrame();
}

}  // namespace rc
