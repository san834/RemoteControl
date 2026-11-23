// 进程 DPI 感知设置（客户端与服务端共用）。
#pragma once

namespace rc {

// 优先设为 Per-Monitor V2（Win10 1703+），失败则回退到 system DPI 感知。
// 必须在任何窗口创建之前调用。
void EnablePerMonitorDpi();

}  // namespace rc
