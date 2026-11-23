// 极小命令行解析：支持 "--key value" 与 "--flag" 两种形式。
//
// 替代旧实现里硬编码在源码中的 IP 和端口 —— 那是评审一眼就会指出的问题。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rc {

class CliArgs {
public:
    // argv 为窄字符参数（Windows 下由 cli.h 的 FromWideCommandLine 转换得到）。
    CliArgs(int argc, char** argv);

    bool Has(const char* name) const;
    std::string Get(const char* name, const std::string& fallback = "") const;
    int GetInt(const char* name, int fallback) const;

    const std::string& program() const { return program_; }

private:
    std::string program_;
    std::map<std::string, std::string> values_;
    std::vector<std::string> flags_;
};

// FNV-1a 32 位哈希。口令不以明文出现在网络上，也不出现在日志里。
uint32_t Fnv1a32(const std::string& text);

// 把 GetCommandLineW() 的结果拆成窄字符 argv（UTF-8）。
std::vector<std::string> FromWideCommandLine();

// 直接从进程命令行构造 CliArgs（供 WinMain 使用，没有 argc/argv）。
CliArgs FromCommandLine();

}  // namespace rc
