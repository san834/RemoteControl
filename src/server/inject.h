// 输入注入抽象。
//
// 旧实现用的是 SetCursorPos + 已废弃的 mouse_event，并且把所有按键都当成
// "按下/抬起"两种状态。这里统一改用 SendInput，并支持：
//   * 绝对坐标（含多显示器的虚拟桌面坐标系）；
//   * 滚轮；
//   * 扫描码 + 扩展键标志，改善输入法/方向键/Alt 组合的兼容性。
#pragma once

#include <cstdint>

#include "rc/protocol.h"

namespace rc {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual void Mouse(const MouseBody& mouse) = 0;
    virtual void Key(const KeyBody& key) = 0;
    virtual const char* name() const = 0;
};

class Win32InputInjector final : public IInputInjector {
public:
    void Mouse(const MouseBody& mouse) override;
    void Key(const KeyBody& key) override;
    const char* name() const override { return "SendInput"; }
};

// 把客户端发来的坐标夹紧到虚拟桌面范围内。
void ClampToVirtualDesktop(int& x, int& y);

}  // namespace rc
