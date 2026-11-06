/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Early boot progress screen for the SerenityOS Linux GUI port.
//
// This deliberately has no dependencies beyond libc + the framebuffer: it is
// started by systemd as soon as /dev/fb0 exists, long before WindowServer and
// the rest of the session, so the user sees something other than a black
// screen. It reads /run/serenity-progress (a number 0..100) and exits when it
// reads 100 or receives SIGTERM.

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t s_quit = 0;

static int max(int a, int b) { return a > b ? a : b; }

void on_signal(int) { s_quit = 1; }

// 5x7 bitmap font, enough for the wordmark and the percentage.
struct Glyph {
    char c;
    unsigned char rows[7];
};

Glyph const g_font[] = {
    // 完整 5x7 大写字母表：之前只收了 "SERENITYOS"/"LINUX GUI PORT" 用到的字母，
    // 阶段文字里出现 C、V、D 就画成空白（"SERVICES" -> "SER I ES"）。
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '%', { 0x19, 0x1A, 0x02, 0x04, 0x08, 0x16, 0x13 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08 } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '/', { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
    { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
    { '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '=', { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 } },
    { ' ', { 0, 0, 0, 0, 0, 0, 0 } },
};

Glyph const* glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    for (auto const& glyph : g_font) {
        if (glyph.c == c)
            return &glyph;
    }
    return nullptr;
}

static void sleep_ms(int ms);

struct Framebuffer {
    int fd { -1 };
    unsigned char* memory { nullptr };
    size_t size { 0 };
    int width { 0 };
    int height { 0 };
    int stride { 0 }; // bytes per line
    int bpp { 32 };

    bool open_device(char const* path)
    {
        fd = ::open(path, O_RDWR);
        if (fd < 0)
            return false;

        struct fb_var_screeninfo var { };
        struct fb_fix_screeninfo fix { };
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0)
            return false;

        // fbdev usually comes up at 640x480; ask for something bigger so the
        // logo is not drawn tiny. Silently keep the default if unsupported.
        var.xres = 1024;
        var.yres = 768;
        var.xres_virtual = 1024;
        var.yres_virtual = 768;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
            var.xres = 640;
            var.yres = 480;
        }

        width = static_cast<int>(var.xres);
        height = static_cast<int>(var.yres);
        bpp = static_cast<int>(var.bits_per_pixel);
        stride = static_cast<int>(fix.line_length);
        size = static_cast<size_t>(stride) * static_cast<size_t>(height);

        memory = static_cast<unsigned char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        return memory != MAP_FAILED;
    }

    void put_pixel(int x, int y, unsigned char r, unsigned char g, unsigned char b)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return;
        if (bpp == 32) {
            auto* p = reinterpret_cast<unsigned int*>(memory + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4);
            *p = (static_cast<unsigned int>(r) << 16) | (static_cast<unsigned int>(g) << 8) | b;
        } else if (bpp == 16) {
            auto* p = reinterpret_cast<unsigned short*>(memory + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 2);
            *p = static_cast<unsigned short>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }

    void fill_rect(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
    {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                put_pixel(xx, yy, r, g, b);
    }

    void draw_glyph(Glyph const& glyph, int x, int y, int scale, unsigned char r, unsigned char g, unsigned char b)
    {
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph.rows[row] & (0x10 >> col))
                    fill_rect(x + col * scale, y + row * scale, scale, scale, r, g, b);
            }
        }
    }

    int draw_text(char const* text, int x, int y, int scale, unsigned char r, unsigned char g, unsigned char b)
    {
        int cursor = x;
        for (auto const* p = text; *p; ++p) {
            auto const* glyph = glyph_for(*p);
            if (!glyph) {
                cursor += 6 * scale;
                continue;
            }
            draw_glyph(*glyph, cursor, y, scale, r, g, b);
            cursor += 6 * scale;
        }
        return cursor - x;
    }

    int text_width(char const* text, int scale)
    {
        int width = 0;
        for (auto const* p = text; *p; ++p)
            width += 6;
        return width * scale;
    }
};

int read_progress(char* stage, size_t stage_size)
{
    // Written by serenitid. When the desktop runs as an unprivileged user the
    // file lives in a RuntimeDirectory (/run/serenity/progress) that systemd
    // owns for that user; keep the legacy /run path as a fallback so a root
    // session (and the classic layout) still works unchanged.
    char const* path = getenv("SERENITY_PROGRESS");
    auto* file = fopen(path && *path ? path : "/run/serenity/progress", "r");
    if (!file)
        return 0;

    int value = 0;
    if (fscanf(file, "%d", &value) != 1)
        value = 0;

    // 可选第二阶段字段：35 启动图形栈
    if (stage && stage_size > 1) {
        stage[0] = '\0';
        int c;
        while ((c = fgetc(file)) == ' ' || c == '\t') { }
        if (c != EOF && c != '\n') {
            size_t length = 0;
            while (c != EOF && c != '\n' && length + 1 < stage_size) {
                stage[length++] = static_cast<char>(c);
                c = fgetc(file);
            }
            stage[length] = '\0';
        }
    }

    fclose(file);
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    return value;
}

void sleep_ms(int ms)
{
    struct timespec ts { ms / 1000, static_cast<long>(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// 背景渐变只画一次：每帧整屏填充会让部分 fbdev 闪。
void draw_background(Framebuffer& fb)
{
    for (int y = 0; y < fb.height; ++y) {
        auto t = static_cast<float>(y) / static_cast<float>(fb.height);
        auto r = static_cast<unsigned char>(0x04 + t * 0x06);
        auto g = static_cast<unsigned char>(0x08 + t * 0x18);
        auto b = static_cast<unsigned char>(0x1c + t * 0x40);
        fb.fill_rect(0, y, fb.width, 1, r, g, b);
    }
}

void draw_spinner(Framebuffer& fb, int center_x, int y, int tick, int scale)
{
    // 12 个点循环点亮：进度不动的时候屏幕上也有东西在动。
    constexpr int dots = 12;
    auto const dot_size = 2 * scale;
    auto const step = 5 * scale;
    auto const start_x = center_x - (dots * step) / 2;

    for (int i = 0; i < dots; ++i) {
        int phase = (tick / 2 - i) % dots;
        if (phase < 0)
            phase += dots;
        // 头部最亮，往后渐暗（尾部的底色别太暗，否则看成一条静线）。
        int level = phase == 0 ? 0xff : (phase < 5 ? 0xc0 - phase * 0x1c : 0x48);
        fb.fill_rect(start_x + i * step, y, dot_size, dot_size,
            static_cast<unsigned char>(level / 3),
            static_cast<unsigned char>(level),
            static_cast<unsigned char>(level));
    }
}

// 只有两样东西：上面的 OS 名字、下面的循环动画。
// 进度条、百分比、阶段文字都去掉了 —— 开机几十秒里它们既看不清也不重要，
// 而进度值仍然用来决定什么时候退出（让 X 接管显示）。
} // namespace

int main()
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    // The framebuffer device usually shows up a few seconds into boot (virtio-gpu
    // fbdev emulation is a module), so wait for it instead of giving up.
    Framebuffer fb;
    bool opened = false;
    for (int attempt = 0; attempt < 150 && !s_quit; ++attempt) {
        if (fb.open_device("/dev/fb0") || fb.open_device("/dev/fb1")) {
            opened = true;
            break;
        }
        sleep_ms(100);
    }
    if (!opened)
        return 1;

    // 背景和标题只画一次：每帧整屏填充在部分 fbdev 上会闪。
    draw_background(fb);

    char const* title = "SERENITYOS";
    // 字号按屏宽取（约屏宽的 55%）：按屏高算会顶到左右边，看着像被裁过。
    int title_scale = max(3, fb.width / 100);
    int title_width = fb.text_width(title, title_scale);
    int title_height = 7 * title_scale;

    int const dot_scale = max(2, title_scale / 2);
    int const dot_size = 2 * dot_scale;

    // 布局：标题在中上（38% 高度），动画在中下（68% 高度）—— 开机画面留白多，
    // 这样比整块居中更像固件画面。
    int const title_x = (fb.width - title_width) / 2;
    int const title_y = fb.height * 38 / 100;

    fb.draw_text(title, title_x, title_y, title_scale, 0xff, 0xff, 0xff);

    // 标题下面那道细分隔线（点阵风格的小装饰）。
    int const rule_width = title_width / 2;
    int const rule_thickness = max(1, title_scale / 6);
    int const rule_y = title_y + title_height + title_scale / 2;
    fb.fill_rect((fb.width - rule_width) / 2, rule_y, rule_width, rule_thickness, 0x30, 0x48, 0x80);

    int const spinner_y = fb.height * 78 / 100;
    int const spinner_scale = dot_scale;

    int tick = 0;
    while (!s_quit) {
        // 只重画动画带：动画带以外是静态像素，避免整屏重绘闪烁。
        int const band_top = spinner_y - dot_size;
        int const band_bottom = spinner_y + dot_size * 2;
        for (int y = band_top; y < band_bottom; ++y) {
            auto t = static_cast<float>(y) / static_cast<float>(fb.height);
            fb.fill_rect(0, y, fb.width, 1,
                static_cast<unsigned char>(0x04 + t * 0x06),
                static_cast<unsigned char>(0x08 + t * 0x18),
                static_cast<unsigned char>(0x1c + t * 0x40));
        }

        draw_spinner(fb, fb.width / 2, spinner_y, tick, spinner_scale);

        ++tick;
        sleep_ms(40);

        // 每 400ms 看一次进度：到 100 就让出显示给图形栈。
        if (tick % 10 == 0 && read_progress(nullptr, 0) >= 100)
            break;
    }

    if (fb.memory)
        munmap(fb.memory, fb.size);
    if (fb.fd >= 0)
        close(fb.fd);
    return 0;
}
