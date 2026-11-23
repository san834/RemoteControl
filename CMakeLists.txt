cmake_minimum_required(VERSION 3.20)
project(RemoteControl VERSION 2.0.0 LANGUAGES CXX)

# 只支持 Windows + MSVC：CaptureBackend 用了 GDI/DIB，InputInjector 用了
# SendInput，协议里的魔数是小端常量。跨平台改造（Linux 的 X11/uinput）留着
# 接口但没实现，见 README 的"已知限制"。

if (NOT WIN32)
    message(FATAL_ERROR "RemoteControl 只能在 Windows 上构建")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # /utf-8: 源码里有中文注释，不加这个 MSVC 会按 GBK 解释导致 C4819。
    # /permissive-: 关闭 MSVC 的宽松模式，暴露非标准写法。
    # /Zc:__cplusplus: 让 __cplusplus 反映真正的标准版本。
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP)
    add_compile_options(/W4)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif ()

# ---------------------------------------------------------------------------
# C 运行库链接方式
#   ON （默认）= /MT，静态链接：exe 只有系统 DLL 依赖，拷到只装了 Windows 的
#                虚拟机上双击就能跑，不需要 VC++ 可再发行组件包。
#   OFF        = /MD，动态链接：exe 更小，但目标机器必须装对应版本的运行时。
# 这个项目的使用场景正是"把服务端拷进虚拟机"，所以默认 ON。
# ---------------------------------------------------------------------------
option(RC_STATIC_RUNTIME "静态链接 MSVC 运行库（/MT），便于拷到裸机运行" ON)

if (RC_STATIC_RUNTIME)
    if (MSVC)
        # CMP0091 下由这个变量统一决定 /MT 还是 /MD，对所有 target 生效。
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif ()
endif ()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 公共层：协议、流式编解码、Socket、压缩、帧差分、日志、命令行
# ---------------------------------------------------------------------------
add_library(rc_common STATIC
    src/common/cli.cpp
    src/common/compress.cpp
    src/common/crc32.cpp
    src/common/dpi.cpp
    src/common/frame.cpp
    src/common/log.cpp
    src/common/net.cpp
    src/common/stream.cpp
)
target_include_directories(rc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rc_common PUBLIC ws2_32 cabinet user32 shell32 Threads::Threads)

# ---------------------------------------------------------------------------
# 服务端
# ---------------------------------------------------------------------------
add_executable(rc_server
    src/server/capture.cpp
    src/server/inject.cpp
    src/server/main.cpp
    src/server/session.cpp
)
target_link_libraries(rc_server PRIVATE rc_common)

# ---------------------------------------------------------------------------
# 客户端（GUI 子系统）
# ---------------------------------------------------------------------------
add_executable(rc_client
    src/client/main.cpp
    src/client/session.cpp
    src/client/surface.cpp
    src/client/window.cpp
)
set_target_properties(rc_client PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(rc_client PRIVATE rc_common gdi32)

# ---------------------------------------------------------------------------
# 测试与基准
# ---------------------------------------------------------------------------
option(RC_BUILD_TESTS "构建单元测试与编码基准" ON)

if (RC_BUILD_TESTS)
    enable_testing()

    add_executable(rc_tests tests/test_main.cpp)
    target_link_libraries(rc_tests PRIVATE rc_common)
    add_test(NAME rc_tests COMMAND rc_tests)

    add_executable(rc_bench tests/bench.cpp)
    target_link_libraries(rc_bench PRIVATE rc_common gdi32 gdiplus)
endif ()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT rc_server)
