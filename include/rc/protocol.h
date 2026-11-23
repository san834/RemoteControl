// 协议定义：客户端与服务端共享的唯一副本。
//
// 设计目标：
//   1. 单一来源 —— 历史上的实现把 PacketHeader / enum 在 client.cpp 和
//      RemoteControl.cpp 里各写了一遍，改协议必然漏改一边。这里只保留一份。
//   2. 定长头 + 变长体，配合 stream.h 的状态机解码器，正常路径 O(1) 解析，
//      不需要在缓冲区里"扫描魔数"。
//   3. 包头自带 CRC32，收发双方都能在第一时间发现损坏，并且能重同步。
#pragma once

#include <cstddef>
#include <cstdint>

namespace rc {

// 'RCV2' 的小端表示。注意：这是小端序的魔数，跨大端平台不兼容（见 README 限制）。
inline constexpr uint32_t kMagic = 0x32564352u;
inline constexpr uint16_t kVersion = 2;

// 单个包体的上限。超过即视为协议损坏/恶意输入，解码器会丢弃并重同步。
// 上限的存在同时防止了 body_len 被伪造时的一次性巨额分配。
inline constexpr uint32_t kMaxBodyBytes = 48u * 1024 * 1024;

// 屏幕分块边长。64 是"差分粒度"与"压缩上下文"的折中：
// 更小块 → 脏数据更精确但分块表开销变大；更大块 → 反之。
inline constexpr uint16_t kDefaultTileSize = 64;

#pragma pack(push, 1)

// 固定 20 字节包头。
struct PacketHeader {
    uint32_t magic;      // kMagic
    uint16_t cmd;        // Cmd
    uint16_t flags;      // PacketFlag 的位或
    uint32_t body_len;   // 包体字节数
    uint32_t seq;        // 发送方自增序号（每方向独立）
    uint32_t crc32;      // 包体的 CRC32；body_len == 0 时为 0
};

enum PacketFlag : uint16_t {
    kFlagNone = 0,
    kFlagCompressed = 1u << 0,      // 包体经 MSZIP 压缩
    kFlagKeyFrame = 1u << 1,        // 整帧，所有分块都是脏块
    kFlagCompressedHuff = 1u << 2,  // 包体经 XPRESS_HUFF 压缩（Win10+）
};

enum class Cmd : uint16_t {
    kHello = 1,         // C -> S
    kHelloAck = 2,      // S -> C，携带远端屏幕几何信息
    kFrameRequest = 3,  // C -> S，空包体
    kFrame = 4,         // S -> C，FrameHead + 分块
    kMouse = 5,         // C -> S
    kKey = 6,           // C -> S
    kPing = 7,          // C -> S，心跳
    kPong = 8,          // S -> C
    kBye = 9,           // 双向，携带断开原因
};

enum class MouseAction : uint8_t {
    kMove = 1,
    kLeftDown,
    kLeftUp,
    kRightDown,
    kRightUp,
    kMiddleDown,
    kMiddleUp,
    kWheel,
};

enum class KeyState : uint8_t {
    kDown = 0,
    kUp = 1,
};

// 握手。token_hash 是口令的 FNV-1a 哈希，0 表示未启用认证。
// client_width/height 让服务端按"客户端的可显示尺寸"决定降采样倍数：
// 把 2560x1600 的桌面原样传给 1024 宽的窗口，等于白传 6 倍像素。
struct HelloBody {
    uint16_t version;
    uint16_t client_width;
    uint16_t client_height;
    uint16_t reserved;
    uint32_t token_hash;
};

// 握手应答。origin_x/origin_y 是虚拟桌面左上角在屏幕坐标系里的偏移，
// 客户端用它把窗口坐标换算成多显示器环境下的绝对坐标。
// scale 是服务端降采样倍数：远端坐标 = origin + 帧坐标 * scale。
struct HelloAckBody {
    uint16_t version;
    uint16_t tile_size;
    int32_t origin_x;
    int32_t origin_y;
    int32_t screen_w;  // 降采样后的帧宽
    int32_t screen_h;
    uint16_t scale;
    uint16_t reserved;
};

struct MouseBody {
    uint8_t action;    // MouseAction
    uint8_t buttons;   // 保留
    int16_t wheel;     // 滚轮增量，原样携带 WM_MOUSEWHEEL 的 1/120 单位
    int32_t x;         // 虚拟桌面坐标
    int32_t y;
};

struct KeyBody {
    uint16_t vk;       // 虚拟键码
    uint16_t scan;     // 扫描码
    uint8_t state;     // KeyState
    uint8_t reserved[3];
};

struct ByeBody {
    uint16_t reason;
    uint16_t reserved;
};

// 帧头。紧跟在包体最前面，之后是 tile_count 个 TileEntry，再之后是像素数据。
// 像素格式统一为 BGRA8888（与 Windows 32bpp DIB 的内存布局一致），自上而下。
struct FrameHead {
    uint32_t frame_id;
    uint16_t screen_w;
    uint16_t screen_h;
    uint16_t tile_size;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t tile_count;      // 本帧携带的脏块数量，0 表示"画面无变化"
    int16_t cursor_x;         // 相对捕获图像的坐标
    int16_t cursor_y;
    uint8_t cursor_visible;
    uint8_t key_frame;        // 1 表示所有分块都是脏块，客户端需整屏刷新
    uint8_t reserved[2];
};

struct TileEntry {
    uint16_t tile_index;      // = ty * tiles_x + tx
    uint16_t width;
    uint16_t height;
    uint32_t data_len;        // width * height * 4
};

#pragma pack(pop)

inline constexpr std::size_t kHeaderSize = sizeof(PacketHeader);

static_assert(sizeof(PacketHeader) == 20, "包头必须保持 20 字节");
static_assert(sizeof(HelloBody) == 12, "HelloBody 布局变化");
static_assert(sizeof(HelloAckBody) == 24, "HelloAckBody 布局变化");
static_assert(sizeof(MouseBody) == 12, "MouseBody 布局变化");
static_assert(sizeof(KeyBody) == 8, "KeyBody 布局变化");
static_assert(sizeof(FrameHead) == 24, "FrameHead 布局变化");
static_assert(sizeof(TileEntry) == 10, "TileEntry 布局变化");

inline constexpr unsigned kByeVersionMismatch = 1;
inline constexpr unsigned kByeAuthFailed = 2;
inline constexpr unsigned kByeProtocolError = 3;

}  // namespace rc
