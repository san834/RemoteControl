// 客户端主窗口。
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "session.h"

namespace rc {

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // 注册窗口类 → 创建窗口 → 建立会话 → 显示窗口 → 启动线程。
    bool Create(HINSTANCE instance, int show_command, const ClientConfig& config);

    void RunMessageLoop();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam);
    LRESULT WndProc(UINT message, WPARAM wparam, LPARAM lparam);

    void OnPaint();
    void DrawStatus(HDC dc, const RECT& client);
    void SendMouse(MouseAction action, int client_x, int client_y,
                   int wheel_units = 0);
    void SendKey(UINT message, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    std::unique_ptr<ClientSession> session_;
    std::wstring title_;
    ULONGLONG last_move_tick_ = 0;
};

}  // namespace rc
