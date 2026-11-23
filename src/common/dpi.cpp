#include "rc/dpi.h"

#include <windows.h>

namespace rc {

void EnablePerMonitorDpi() {
    using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto set_context = reinterpret_cast<SetDpiContextFn>(
            reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (set_context &&
            set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    // Win7/8 回退：进程级（system）DPI 感知。
    SetProcessDPIAware();
}

}  // namespace rc
