#include "rc/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace rc::log {
namespace {

constexpr int kMaxMessage = 2048;

std::mutex g_mutex;
FILE* g_sink = nullptr;
bool g_owns_sink = false;
Level g_level = Level::kInfo;

const char* LevelName(Level level) {
    switch (level) {
        case Level::kDebug: return "DEBUG";
        case Level::kInfo: return "INFO ";
        case Level::kWarn: return "WARN ";
        case Level::kError: return "ERROR";
    }
    return "INFO ";
}

// 调用方必须持有 g_mutex。
void EmitLocked(const std::string& text) {
    if (!g_sink) g_sink = stdout;

    // 仅在"stdout 且它确实是控制台"时走宽字符路径；重定向到文件时
    // 仍然写 UTF-8 字节，保证日志文件可被正常编辑器读取。
    if (g_sink == stdout) {
        const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (console && console != INVALID_HANDLE_VALUE &&
            GetConsoleMode(console, &mode)) {
            const int wide_len = MultiByteToWideChar(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                0);
            if (wide_len > 0) {
                std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                    static_cast<int>(text.size()), wide.data(),
                                    wide_len);
                DWORD written = 0;
                if (WriteConsoleW(console, wide.data(),
                                  static_cast<DWORD>(wide.size()), &written,
                                  nullptr)) {
                    return;
                }
                // 宽字符路径失败就退回到字节流，至少不会丢日志。
            }
        }
    }

    std::fwrite(text.data(), 1, text.size(), g_sink);
    std::fflush(g_sink);
}

void WriteV(Level level, const char* fmt, va_list args) {
    if (!fmt) return;

    char message[kMaxMessage];
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (level < g_level) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char prefix[128];
        std::snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03d][%s][T%lu] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                      LevelName(level),
                      static_cast<unsigned long>(GetCurrentThreadId()));

        std::string line(prefix);
        line += message;
        line += '\n';
        EmitLocked(line);
    }

    // GUI 进程没有控制台时也能在调试器里看到。
    if (IsDebuggerPresent()) {
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
}

}  // namespace

void Init(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_owns_sink && g_sink) {
        std::fclose(g_sink);
    }
    g_sink = nullptr;
    g_owns_sink = false;
    if (!path || !*path) {
        g_sink = stdout;
        return;
    }
    // "w" 而不是 "a"：每次启动覆盖，避免日志无限增长。
    if (fopen_s(&g_sink, path, "w") != 0 || !g_sink) {
        g_sink = stdout;
        return;
    }
    g_owns_sink = true;
}

void SetLevel(Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

void Write(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
    va_end(args);
}

void Raw(const char* text) {
    if (!text) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string chunk(text);
    if (chunk.empty() || chunk.back() != '\n') chunk += '\n';
    EmitLocked(chunk);
}

void Debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(Level::kDebug, fmt, args);
    va_end(args);
}

void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(Level::kInfo, fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(Level::kWarn, fmt, args);
    va_end(args);
}

void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(Level::kError, fmt, args);
    va_end(args);
}

}  // namespace rc::log
