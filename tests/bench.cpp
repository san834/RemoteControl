// 编码基准：用同一组合成桌面，对比三种屏幕传输方案。
//
//   1. 整帧 PNG          —— 本项目 v1 的做法（BitBlt → CImage::Save PNG）
//   2. 整帧 MSZIP        —— 只换压缩算法，不换模型
//   3. 分块脏矩形 + MSZIP —— 本项目 v2 的做法
//
// 目的：README 里的性能数字必须能一条命令复现，而不是凭感觉写。
#include <windows.h>
#include <atlimage.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rc/compress.h"
#include "rc/frame.h"

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFrames = 40;
constexpr int kIdleFrames = 20;

double NowUs() {
    using namespace std::chrono;
    return static_cast<double>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch())
            .count());
}

// 造一个"像桌面"的画面：静态壁纸 + 一个会移动的窗口 + 一个变化的进度条。
class SyntheticDesktop {
public:
    SyntheticDesktop()
        : wallpaper_(static_cast<std::size_t>(kWidth) * kHeight * 4) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(y) * kWidth + x) * 4;
                wallpaper_[i + 0] = static_cast<uint8_t>((x * 255) / kWidth);
                wallpaper_[i + 1] = static_cast<uint8_t>((y * 255) / kHeight);
                wallpaper_[i + 2] = static_cast<uint8_t>((x + y) & 0x7F);
                wallpaper_[i + 3] = 0xFF;
            }
        }
    }

    // 第 n 帧：窗口平移 + 进度条推进 + 一个小光标块移动。
    std::vector<uint8_t> Frame(int n) const {
        std::vector<uint8_t> frame = wallpaper_;
        const int win_x = 120 + (n * 11) % (kWidth - 900);
        const int win_y = 90;

        Fill(frame, win_x, win_y, 800, 560, 0xF0, 0xF0, 0xF0);
        Fill(frame, win_x, win_y, 800, 36, 0x2D, 0x6C, 0xDF);
        for (int line = 0; line < 12; ++line) {
            const int w = 120 + ((line * 73) % 520);
            Fill(frame, win_x + 28, win_y + 70 + line * 32, w, 12, 0x60, 0x60,
                 0x60);
        }
        const int progress = (n * 17) % 600;
        Fill(frame, win_x + 28, win_y + 500, 600, 18, 0xD0, 0xD0, 0xD0);
        Fill(frame, win_x + 28, win_y + 500, progress, 18, 0x2D, 0xC0, 0x6C);

        const int cx = 300 + (n * 23) % (kWidth - 400);
        const int cy = 200 + (n * 13) % (kHeight - 300);
        Fill(frame, cx, cy, 16, 24, 0x00, 0x00, 0x00);
        return frame;
    }

    // 完全静止的帧（用来量化"用户没有操作时"的开销）。
    std::vector<uint8_t> StaticFrame() const {
        std::vector<uint8_t> frame = wallpaper_;
        Fill(frame, 320, 180, 800, 560, 0xF0, 0xF0, 0xF0);
        Fill(frame, 320, 180, 800, 36, 0x2D, 0x6C, 0xDF);
        return frame;
    }

private:
    static void Fill(std::vector<uint8_t>& buffer, int x0, int y0, int w, int h,
                     uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y < y0 + h && y < kHeight; ++y) {
            if (y < 0) continue;
            for (int x = x0; x < x0 + w && x < kWidth; ++x) {
                if (x < 0) continue;
                const std::size_t i =
                    (static_cast<std::size_t>(y) * kWidth + x) * 4;
                buffer[i + 0] = b;
                buffer[i + 1] = g;
                buffer[i + 2] = r;
                buffer[i + 3] = 0xFF;
            }
        }
    }

    std::vector<uint8_t> wallpaper_;
};

// 用 ATL CImage 把 BGRA 缓冲编码成 PNG，返回字节数与耗时。
bool EncodePng(const std::vector<uint8_t>& bgra, std::size_t* out_bytes,
               double* out_us) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC src_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    void* bits = nullptr;
    HBITMAP src =
        CreateDIBSection(src_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!src || !bits) {
        DeleteDC(src_dc);
        return false;
    }
    std::memcpy(bits, bgra.data(), bgra.size());
    SelectObject(src_dc, src);

    const double start = NowUs();
    CImage image;
    image.Create(kWidth, kHeight, 32);
    HDC dst = image.GetDC();
    BitBlt(dst, 0, 0, kWidth, kHeight, src_dc, 0, 0, SRCCOPY);
    image.ReleaseDC();

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, 0);
    IStream* stream = nullptr;
    bool ok = false;
    if (memory && SUCCEEDED(CreateStreamOnHGlobal(memory, TRUE, &stream)) &&
        stream) {
        if (SUCCEEDED(image.Save(stream, Gdiplus::ImageFormatPNG))) {
            STATSTG stat{};
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
                *out_bytes = static_cast<std::size_t>(stat.cbSize.QuadPart);
                ok = true;
            }
        }
        stream->Release();
    }
    *out_us = NowUs() - start;

    image.Destroy();
    DeleteObject(src);
    DeleteDC(src_dc);
    return ok;
}

struct TimedResult {
    double bytes = 0;
    double us = 0;
    double tiles = 0;
    long frames = 0;

    double avg_bytes() const { return frames ? bytes / frames : 0; }
    double avg_us() const { return frames ? us / frames : 0; }
};

void PrintRow(const char* name, const TimedResult& result, double png_bytes) {
    if (result.frames == 0) return;
    const double bytes = result.avg_bytes();
    const double us = result.avg_us();
    const double ratio = png_bytes > 0 ? bytes / png_bytes * 100.0 : 100.0;
    std::printf("| %-24s | %9.1f KB | %6.1f%% | %7.2f ms | %6.1f fps |\n", name,
                bytes / 1024.0, ratio, us / 1000.0,
                us > 0 ? 1000000.0 / us : 0.0);
}

rc::FrameImage View(const std::vector<uint8_t>& pixels) {
    rc::FrameImage image;
    image.width = kWidth;
    image.height = kHeight;
    image.pixels = pixels.data();
    image.cursor_visible = true;
    image.cursor_x = kWidth / 2;
    image.cursor_y = kHeight / 2;
    return image;
}

}  // namespace

int main() {
    std::printf("== rc_bench: %dx%d, %d 帧合成桌面 ==\n\n", kWidth, kHeight,
                kFrames);

    const rc::Compression algorithm = rc::PreferredCompression();
    if (algorithm == rc::Compression::kNone) {
        std::puts("当前系统缺少压缩 API（需要 Win8+），无法继续。");
        return 1;
    }
    std::printf("压缩算法: %s\n\n",
                algorithm == rc::Compression::kXpressHuff ? "XPRESS_HUFF"
                                                          : "MSZIP");

    const SyntheticDesktop desktop;
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(kFrames);
    for (int i = 0; i < kFrames; ++i) frames.push_back(desktop.Frame(i));

    TimedResult png;
    TimedResult raw_zip;
    TimedResult delta_zip;

    // --- 1. 整帧 PNG ---
    for (const auto& frame : frames) {
        std::size_t bytes = 0;
        double us = 0;
        if (!EncodePng(frame, &bytes, &us)) {
            std::puts("PNG 编码失败（GDI+/ATL 不可用）");
            return 2;
        }
        png.bytes += static_cast<double>(bytes);
        png.us += us;
        ++png.frames;
    }

    // --- 2. 整帧压缩（只换算法，不换模型）---
    for (const auto& frame : frames) {
        std::vector<uint8_t> compressed;
        const double start = NowUs();
        if (!rc::CompressData(frame.data(), frame.size(), algorithm, compressed)) continue;
        raw_zip.us += NowUs() - start;
        raw_zip.bytes += static_cast<double>(compressed.size());
        ++raw_zip.frames;
    }

    // --- 3. 分块脏矩形 + 压缩 ---
    rc::FrameEncoder encoder(rc::kDefaultTileSize);
    double diff_us = 0;
    double zip_us = 0;
    for (const auto& frame : frames) {
        const rc::FrameImage image = View(frame);

        const double t0 = NowUs();
        const rc::EncodedFrame encoded = encoder.Encode(image);
        const double t1 = NowUs();

        std::vector<uint8_t> compressed;
        std::size_t wire = encoded.body.size();
        if (rc::CompressData(encoded.body.data(), encoded.body.size(),
                             algorithm, compressed) &&
            compressed.size() + 32 < encoded.body.size()) {
            wire = compressed.size();
        }
        const double t2 = NowUs();

        diff_us += t1 - t0;
        zip_us += t2 - t1;
        delta_zip.us += t2 - t0;
        delta_zip.bytes += static_cast<double>(wire);
        delta_zip.tiles += encoded.tiles_dirty;
        ++delta_zip.frames;
    }

    // --- 4. 静止画面 ---
    const std::vector<uint8_t> still = desktop.StaticFrame();
    encoder.Encode(View(still));  // 先让编码器认识这一帧
    TimedResult idle;
    for (int i = 0; i < kIdleFrames; ++i) {
        const double t0 = NowUs();
        const rc::EncodedFrame encoded = encoder.Encode(View(still));
        const double t1 = NowUs();
        idle.bytes += static_cast<double>(encoded.body.size());
        idle.tiles += encoded.tiles_dirty;
        idle.us += t1 - t0;
        ++idle.frames;
    }

    const double png_avg = png.avg_bytes();

    std::puts("| 方案 | 平均每帧 | 相对 PNG | 平均耗时 | 单核编码上限 |");
    std::puts("|:--|--:|--:|--:|--:|");
    PrintRow("整帧 PNG (v1)", png, png_avg);
    PrintRow("整帧 + 压缩（不差分）", raw_zip, png_avg);
    PrintRow("分块脏块 + 压缩 (v2)", delta_zip, png_avg);
    std::printf("| %-24s | %6.0f B  | %6.3f%% | %7.3f ms | %6.0f fps |\n",
                "静止画面 (v2)", idle.avg_bytes(), idle.avg_bytes() / png_avg * 100.0,
                idle.avg_us() / 1000.0,
                idle.avg_us() > 0 ? 1000000.0 / idle.avg_us() : 0.0);

    std::printf(
        "\nv2 耗时构成：差分 %.2f ms + 压缩 %.2f ms；平均脏块 %.1f / 510"
        "（%d×%d，分块 %u）。\n",
        diff_us / delta_zip.frames / 1000.0, zip_us / delta_zip.frames / 1000.0,
        delta_zip.tiles / delta_zip.frames,
        (kWidth + rc::kDefaultTileSize - 1) / rc::kDefaultTileSize,
        (kHeight + rc::kDefaultTileSize - 1) / rc::kDefaultTileSize,
        rc::kDefaultTileSize);
    std::puts(
        "单核编码上限 = 1000 / 平均耗时，不含网络往返、捕获（BitBlt）与输入注入。");
    return 0;
}
