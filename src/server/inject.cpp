#include "inject.h"

#include <windows.h>

namespace rc {
namespace {

bool IsExtendedKey(WORD vk) {
    switch (vk) {
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_SNAPSHOT:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

}  // namespace

void ClampToVirtualDesktop(int& x, int& y) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1;
    const int bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
    if (x < left) x = left;
    if (x > right) x = right;
    if (y < top) y = top;
    if (y > bottom) y = bottom;
}

void Win32InputInjector::Mouse(const MouseBody& mouse) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;

    int x = mouse.x;
    int y = mouse.y;
    ClampToVirtualDesktop(x, y);

    // SendInput 的绝对坐标是 0..65535 的归一化值，配合 VIRTUALDESK
    // 覆盖整个虚拟桌面（多显示器）。
    const LONG nx = static_cast<LONG>(
        static_cast<long long>(x - vx) * 65535 / (vw - 1));
    const LONG ny = static_cast<LONG>(
        static_cast<long long>(y - vy) * 65535 / (vh - 1));

    INPUT inputs[3] = {};
    int count = 0;

    // 任何鼠标事件都先同步一次绝对位置，避免"先点后移"的错位。
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dx = nx;
    inputs[count].mi.dy = ny;
    inputs[count].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                               MOUSEEVENTF_VIRTUALDESK;
    ++count;

    auto add_button = [&](DWORD flags) {
        if (count >= 3) return;
        INPUT& in = inputs[count++];
        in.type = INPUT_MOUSE;
        in.mi.dx = nx;
        in.mi.dy = ny;
        in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    };

    switch (static_cast<MouseAction>(mouse.action)) {
        case MouseAction::kMove:
            break;
        case MouseAction::kLeftDown:
            add_button(MOUSEEVENTF_LEFTDOWN);
            break;
        case MouseAction::kLeftUp:
            add_button(MOUSEEVENTF_LEFTUP);
            break;
        case MouseAction::kRightDown:
            add_button(MOUSEEVENTF_RIGHTDOWN);
            break;
        case MouseAction::kRightUp:
            add_button(MOUSEEVENTF_RIGHTUP);
            break;
        case MouseAction::kMiddleDown:
            add_button(MOUSEEVENTF_MIDDLEDOWN);
            break;
        case MouseAction::kMiddleUp:
            add_button(MOUSEEVENTF_MIDDLEUP);
            break;
        case MouseAction::kWheel: {
            if (count >= 3) break;
            INPUT& in = inputs[count++];
            in.type = INPUT_MOUSE;
            in.mi.dx = nx;
            in.mi.dy = ny;
            // mouse.wheel 直接携带客户端的滚轮增量（单位 1/120 格）。
            in.mi.mouseData = static_cast<DWORD>(
                static_cast<short>(mouse.wheel));
            in.mi.dwFlags =
                MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            break;
        }
        default:
            break;
    }

    SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
}

void Win32InputInjector::Key(const KeyBody& key) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = key.vk;
    in.ki.wScan = key.scan;

    DWORD flags = 0;
    if (static_cast<KeyState>(key.state) == KeyState::kUp) {
        flags |= KEYEVENTF_KEYUP;
    }
    if (IsExtendedKey(key.vk)) {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }
    in.ki.dwFlags = flags;

    SendInput(1, &in, sizeof(INPUT));
}

}  // namespace rc
