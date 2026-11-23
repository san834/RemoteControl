// 极简日志：带时间戳 + 线程无关的互斥输出。
//
// 关于中文乱码：日志文本在源码里是 UTF-8，如果直接 fwrite 到控制台，中文
// Windows 的控制台代码页是 936(GBK)，就会显示成乱码。这里两条路径分开：
//   * stdout 是控制台 → 转成 UTF-16 用 WriteConsoleW 输出，完全绕开代码页；
//   * stdout 被重定向到文件/管道 → 按原样写 UTF-8 字节。
#pragma once

#include <cstdio>

namespace rc::log {

enum class Level { kDebug = 0, kInfo, kWarn, kError };

// path 为空则输出到 stdout；否则写入文件（GUI 程序没有控制台）。
void Init(const char* path = nullptr);
void SetLevel(Level level);
void Write(Level level, const char* fmt, ...);

// 原样输出、不加时间戳，用于 --help 这类多行文本。
void Raw(const char* text);

void Debug(const char* fmt, ...);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

}  // namespace rc::log
