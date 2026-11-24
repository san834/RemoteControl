// 单元测试：协议编解码、帧差分/还原、压缩、命令行解析。
//
// 没有引入 gtest：这个项目的目标是"clone + 装 VS 就能跑"，多一个第三方依赖
// 会让 CI 和本地构建都变复杂。断言宏够用。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rc/cli.h"
#include "rc/compress.h"
#include "rc/crc32.h"
#include "rc/frame.h"
#include "rc/log.h"
#include "rc/stream.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            rc::log::Print("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++g_checks;                                                           \
        const auto lhs = (a);                                                 \
        const auto rhs = (b);                                                 \
        if (!(lhs == rhs)) {                                                  \
            rc::log::Print("  [FAIL] %s:%d  %s == %s  (%lld vs %lld)\n",         \
                        __FILE__, __LINE__, #a, #b,                           \
                        static_cast<long long>(lhs),                          \
                        static_cast<long long>(rhs));                         \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        ++g_checks;                                                            \
        const std::string lhs = (a);                                           \
        const std::string rhs = (b);                                           \
        if (lhs != rhs) {                                                      \
            rc::log::Print("  [FAIL] %s:%d  %s == %s  (\"%s\" vs \"%s\")\n",       \
                        __FILE__, __LINE__, #a, #b, lhs.c_str(), rhs.c_str()); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------
void TestCrc32() {
    rc::log::PrintLine("Crc32");
    const char* text = "123456789";
    // IEEE 802.3 标准校验值。
    CHECK_EQ(rc::Crc32(text, std::strlen(text)), 0xCBF43926u);
    CHECK_EQ(rc::Crc32("", 0), 0u);

    // 增量计算必须与一次性计算一致。
    const uint32_t whole = rc::Crc32(text, 9);
    const uint32_t first = rc::Crc32(text, 4);
    CHECK_EQ(rc::Crc32(text + 4, 5, first), whole);
}

// ---------------------------------------------------------------------------
// 流式解码
// ---------------------------------------------------------------------------
void TestEncodeDecodeRoundTrip() {
    rc::log::PrintLine("Encode / Decode 往返");
    rc::MouseBody mouse{};
    mouse.action = static_cast<uint8_t>(rc::MouseAction::kLeftDown);
    mouse.x = 1234;
    mouse.y = -56;
    mouse.wheel = 120;

    const std::vector<uint8_t> packet =
        rc::Encode(rc::Cmd::kMouse, 7, 0, &mouse, sizeof(mouse));
    CHECK_EQ(packet.size(), rc::kHeaderSize + sizeof(mouse));

    rc::Decoder decoder;
    decoder.Push(packet.data(), packet.size());
    rc::Message message;
    CHECK(decoder.Pop(message));
    CHECK(message.cmd == rc::Cmd::kMouse);
    CHECK_EQ(message.seq, 7u);

    rc::MouseBody back{};
    CHECK(message.As(back));
    CHECK_EQ(back.x, 1234);
    CHECK_EQ(back.y, -56);
    CHECK_EQ(back.wheel, 120);
    CHECK(!decoder.Pop(message));  // 只有一条
}

void TestByteAtATime() {
    rc::log::PrintLine("逐字节投递（最极端的半包）");
    const std::vector<uint8_t> a = rc::Encode(rc::Cmd::kPing, 1, 0, nullptr, 0);
    const std::vector<uint8_t> b =
        rc::Encode(rc::Cmd::kFrameRequest, 2, 0, nullptr, 0);

    std::vector<uint8_t> stream;
    stream.insert(stream.end(), a.begin(), a.end());
    stream.insert(stream.end(), b.begin(), b.end());

    rc::Decoder decoder;
    int decoded = 0;
    for (uint8_t byte : stream) {
        decoder.Push(&byte, 1);
        rc::Message message;
        while (decoder.Pop(message)) ++decoded;
    }
    CHECK_EQ(decoded, 2);
    CHECK_EQ(decoder.stats().packets, 2u);
    CHECK_EQ(decoder.stats().resyncs, 0u);
}

void TestMultiplePacketsInOneChunk() {
    rc::log::PrintLine("一次投递包含多个包（粘包）");
    std::vector<uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        const std::vector<uint8_t> packet =
            rc::Encode(rc::Cmd::kPing, static_cast<uint32_t>(i), 0, nullptr, 0);
        stream.insert(stream.end(), packet.begin(), packet.end());
    }

    rc::Decoder decoder;
    decoder.Push(stream.data(), stream.size());
    int decoded = 0;
    rc::Message message;
    while (decoder.Pop(message)) {
        CHECK_EQ(message.seq, static_cast<uint32_t>(decoded));
        ++decoded;
    }
    CHECK_EQ(decoded, 5);
}

void TestResyncAfterGarbage() {
    rc::log::PrintLine("脏数据前缀后重同步");
    std::vector<uint8_t> stream(37, 0xAA);  // 不含魔数的垃圾
    const std::vector<uint8_t> packet =
        rc::Encode(rc::Cmd::kPong, 9, 0, nullptr, 0);
    stream.insert(stream.end(), packet.begin(), packet.end());

    rc::Decoder decoder;
    decoder.Push(stream.data(), stream.size());
    rc::Message message;
    CHECK(decoder.Pop(message));
    CHECK_EQ(decoder.stats().dropped_bytes, 37u);
    CHECK_EQ(decoder.stats().resyncs, 1u);
}

void TestCrcMismatchRecovers() {
    rc::log::PrintLine("包体损坏后丢弃单包并恢复");
    std::vector<uint8_t> bad =
        rc::Encode(rc::Cmd::kMouse, 1, 0, "hello world", 11);
    bad[rc::kHeaderSize + 3] ^= 0xFF;  // 破坏包体

    std::vector<uint8_t> good = rc::Encode(rc::Cmd::kPing, 2, 0, nullptr, 0);
    std::vector<uint8_t> stream = bad;
    stream.insert(stream.end(), good.begin(), good.end());

    rc::Decoder decoder;
    decoder.Push(stream.data(), stream.size());

    rc::Message message;
    int decoded = 0;
    while (decoder.Pop(message)) {
        ++decoded;
        CHECK(message.cmd == rc::Cmd::kPing);
    }
    CHECK_EQ(decoded, 1);
    CHECK(decoder.stats().crc_mismatch >= 1u);
}

void TestOversizedBodyRejected() {
    rc::log::PrintLine("越界 body_len 被拒绝");
    rc::Decoder decoder(1024);

    rc::PacketHeader header{};
    header.magic = rc::kMagic;
    header.cmd = static_cast<uint16_t>(rc::Cmd::kFrame);
    header.body_len = 100000;  // 远超 1024
    header.seq = 1;
    header.crc32 = 0;

    std::vector<uint8_t> stream(sizeof(header) + 16, 0x11);
    std::memcpy(stream.data(), &header, sizeof(header));

    decoder.Push(stream.data(), stream.size());
    rc::Message message;
    CHECK(!decoder.Pop(message));
    CHECK(decoder.stats().resyncs >= 1u);
}

// ---------------------------------------------------------------------------
// 帧编码
// ---------------------------------------------------------------------------
std::vector<uint8_t> MakeFrame(int width, int height, uint8_t seed) {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[i + 0] = static_cast<uint8_t>((x * 3 + seed) & 0xFF);
            pixels[i + 1] = static_cast<uint8_t>((y * 5 + seed) & 0xFF);
            pixels[i + 2] = static_cast<uint8_t>((x + y + seed) & 0xFF);
            pixels[i + 3] = 0xFF;
        }
    }
    return pixels;
}

rc::FrameImage View(const std::vector<uint8_t>& pixels, int width, int height) {
    rc::FrameImage image;
    image.width = width;
    image.height = height;
    image.pixels = pixels.data();
    return image;
}

void TestFirstFrameIsKeyFrame() {
    rc::log::PrintLine("首帧必须是关键帧");
    const int w = 200;
    const int h = 130;
    auto pixels = MakeFrame(w, h, 0);
    rc::FrameEncoder encoder(64);
    const rc::EncodedFrame frame = encoder.Encode(View(pixels, w, h));

    CHECK(frame.key_frame);
    CHECK_EQ(frame.tiles_total, 4 * 3);  // ceil(200/64)=4, ceil(130/64)=3
    CHECK_EQ(frame.tiles_dirty, 12);
    CHECK(!frame.body.empty());
}

void TestUnchangedFrameHasNoTiles() {
    rc::log::PrintLine("画面静止 → 零脏块");
    const int w = 128;
    const int h = 128;
    auto pixels = MakeFrame(w, h, 7);
    rc::FrameEncoder encoder(64);
    encoder.Encode(View(pixels, w, h));  // 关键帧

    const rc::EncodedFrame second = encoder.Encode(View(pixels, w, h));
    CHECK(!second.key_frame);
    CHECK_EQ(second.tiles_dirty, 0);
    CHECK_EQ(second.raw_bytes, 0u);
    CHECK_EQ(second.body.size(), sizeof(rc::FrameHead));
}

void TestSingleTileChange() {
    rc::log::PrintLine("单像素变化 → 只有 1 个脏块");
    const int w = 200;
    const int h = 130;
    auto pixels = MakeFrame(w, h, 3);
    rc::FrameEncoder encoder(64);
    encoder.Encode(View(pixels, w, h));

    // 命中 tile (tx=1, ty=0) 内的一个像素。
    const std::size_t offset = (static_cast<std::size_t>(5) * w + 70) * 4;
    pixels[offset + 0] ^= 0xFF;

    const rc::EncodedFrame second = encoder.Encode(View(pixels, w, h));
    CHECK_EQ(second.tiles_dirty, 1);
    CHECK_EQ(second.raw_bytes, 64u * 64u * 4u);

    rc::FrameDecoder decoder;
    CHECK(decoder.Begin(second.body.data(), second.body.size()));
    CHECK_EQ(decoder.head().tile_count, 1);
    CHECK(!decoder.key_frame());
}

void TestFrameRoundTrip() {
    rc::log::PrintLine("编码 → 解码 → 像素完全一致");
    const int w = 200;
    const int h = 130;
    auto pixels = MakeFrame(w, h, 11);

    rc::FrameEncoder encoder(64);
    const rc::EncodedFrame key = encoder.Encode(View(pixels, w, h));

    // 模拟客户端表面：先全零，再依次叠加。
    std::vector<uint8_t> restored(static_cast<std::size_t>(w) * h * 4, 0);

    rc::FrameDecoder decoder;
    CHECK(decoder.Begin(key.body.data(), key.body.size()));
    rc::FrameDecoder::Target target{restored.data(),
                                    static_cast<std::size_t>(w) * 4};
    CHECK(decoder.BlitTo(target));
    CHECK(std::memcmp(restored.data(), pixels.data(), pixels.size()) == 0);

    // 再来一帧增量。
    for (int i = 0; i < 500; ++i) {
        pixels[(static_cast<std::size_t>(i) * 97) % pixels.size()] ^= 0x5A;
    }
    const rc::EncodedFrame delta = encoder.Encode(View(pixels, w, h));
    CHECK(!delta.key_frame);
    CHECK(delta.tiles_dirty > 0);

    CHECK(decoder.Begin(delta.body.data(), delta.body.size()));
    CHECK(decoder.BlitTo(target));
    CHECK(std::memcmp(restored.data(), pixels.data(), pixels.size()) == 0);
}

void TestCorruptFrameRejected() {
    rc::log::PrintLine("帧体被截断时拒绝解码");
    const int w = 128;
    const int h = 128;
    auto pixels = MakeFrame(w, h, 1);
    rc::FrameEncoder encoder(64);
    const rc::EncodedFrame key = encoder.Encode(View(pixels, w, h));

    std::vector<uint8_t> truncated = key.body;
    truncated.resize(truncated.size() - 10);

    rc::FrameDecoder decoder;
    CHECK(!decoder.Begin(truncated.data(), truncated.size()));
}

// ---------------------------------------------------------------------------
// 压缩
// ---------------------------------------------------------------------------
void TestCompressionRoundTrip() {
    const int w = 300;
    const int h = 200;
    auto pixels = MakeFrame(w, h, 5);
    pixels.resize(pixels.size(), 0);

    const rc::Compression algorithms[] = {rc::Compression::kXpressHuff,
                                          rc::Compression::kMsZip};
    for (const rc::Compression algorithm : algorithms) {
        const bool is_huff = algorithm == rc::Compression::kXpressHuff;
        rc::log::Print("%s 压缩往返\n", is_huff ? "XPRESS_HUFF" : "MSZIP");
        if (!rc::CompressionAvailable(algorithm)) {
            rc::log::PrintLine("  [SKIP] 当前系统不支持该算法");
            continue;
        }

        std::vector<uint8_t> compressed;
        CHECK(rc::CompressData(pixels.data(), pixels.size(), algorithm,
                               compressed));
        CHECK(compressed.size() < pixels.size());

        std::vector<uint8_t> restored;
        CHECK(rc::DecompressData(compressed.data(), compressed.size(), algorithm,
                                 pixels.size() * 2, restored));
        CHECK_EQ(restored.size(), pixels.size());
        CHECK(std::memcmp(restored.data(), pixels.data(), pixels.size()) == 0);

        // 伪造的超大输出必须被拒绝，而不是尝试分配。
        std::vector<uint8_t> too_small_limit;
        CHECK(!rc::DecompressData(compressed.data(), compressed.size(),
                                  algorithm, 16, too_small_limit));
    }
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------
void TestCliArgs() {
    rc::log::PrintLine("命令行解析");
    const char* argv[] = {"rc_server", "--port", "9998", "--verbose", "--token",
                          "secret"};
    rc::CliArgs args(6, const_cast<char**>(argv));
    CHECK_EQ(args.GetInt("port", 9999), 9998);
    CHECK(args.Has("verbose"));
    CHECK_STR(args.Get("token"), "secret");
    CHECK_EQ(args.GetInt("missing", 42), 42);
    CHECK_STR(args.Get("missing", "fallback"), "fallback");
    CHECK_EQ(rc::Fnv1a32(""), 0u);
    CHECK(rc::Fnv1a32("a") != rc::Fnv1a32("b"));
}

}  // namespace

int main() {
    rc::log::PrintLine("== RemoteControl 单元测试 ==");
    TestCrc32();
    TestEncodeDecodeRoundTrip();
    TestByteAtATime();
    TestMultiplePacketsInOneChunk();
    TestResyncAfterGarbage();
    TestCrcMismatchRecovers();
    TestOversizedBodyRejected();
    TestFirstFrameIsKeyFrame();
    TestUnchangedFrameHasNoTiles();
    TestSingleTileChange();
    TestFrameRoundTrip();
    TestCorruptFrameRejected();
    TestCompressionRoundTrip();
    TestCliArgs();

    rc::log::Print("\n共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
