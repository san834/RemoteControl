#include "rc/cli.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>

namespace rc {
namespace {

bool StartsWithDashDash(const std::string& text) {
    return text.size() > 2 && text[0] == '-' && text[1] == '-';
}

}  // namespace

CliArgs::CliArgs(int argc, char** argv) {
    if (argc > 0 && argv && argv[0]) program_ = argv[0];
    for (int i = 1; i < argc; ++i) {
        const std::string token = argv[i] ? argv[i] : "";
        if (!StartsWithDashDash(token)) continue;
        const std::string key = token.substr(2);
        const bool has_value = (i + 1 < argc) && argv[i + 1] &&
                               !StartsWithDashDash(argv[i + 1] ? argv[i + 1] : "");
        if (has_value) {
            values_[key] = argv[++i];
        } else {
            flags_.push_back(key);
        }
    }
}

bool CliArgs::Has(const char* name) const {
    if (!name) return false;
    return values_.count(name) != 0 ||
           std::find(flags_.begin(), flags_.end(), name) != flags_.end();
}

std::string CliArgs::Get(const char* name, const std::string& fallback) const {
    if (!name) return fallback;
    const auto it = values_.find(name);
    return it == values_.end() ? fallback : it->second;
}

int CliArgs::GetInt(const char* name, int fallback) const {
    const std::string text = Get(name);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return fallback;
    return static_cast<int>(value);
}

uint32_t Fnv1a32(const std::string& text) {
    if (text.empty()) return 0;
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

std::vector<std::string> FromWideCommandLine() {
    std::vector<std::string> result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return result;
    for (int i = 0; i < argc; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0,
                                             nullptr, nullptr);
        std::string text(need > 0 ? static_cast<std::size_t>(need - 1) : 0, '\0');
        if (need > 1) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), need,
                                nullptr, nullptr);
        }
        result.push_back(std::move(text));
    }
    LocalFree(argv);
    return result;
}

CliArgs FromCommandLine() {
    std::vector<std::string> args = FromWideCommandLine();
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) argv.push_back(arg.data());
    return CliArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace rc
