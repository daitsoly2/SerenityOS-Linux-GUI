/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A Win9x-flavoured boot splash for the Linux GUI port: dark blue gradient,
// wordmark and a chunky segmented progress bar. It runs as a normal window on
// top of WindowServer, then exits and lets the session script start Taskbar.

#include <AK/Format.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGUI/Application.h>
#include <LibGUI/Event.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

namespace {

constexpr int SPLASH_WIDTH = 640;
constexpr int SPLASH_HEIGHT = 400;
constexpr int SEGMENTS = 26;

class SplashWidget final : public GUI::Widget {
    C_OBJECT(SplashWidget)

public:
    SplashWidget() = default;
    virtual ~SplashWidget() override = default;

    void set_progress(int progress)
    {
        m_progress = progress;
        update();
    }

    void set_status(String text)
    {
        m_status = move(text);
        update();
    }

private:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);

        GUI::Painter painter(*this);
        auto full = rect().to_type<float>();
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(),
            Color::from_rgb(0x000028),
            Color::from_rgb(0x002080));

        // Subtle scanline texture, very 1995.
        for (int y = 0; y < height(); y += 4)
            painter.fill_rect({ 0, y, width(), 1 }, Color::from_argb(0x30000000));

        auto& default_font = Gfx::FontDatabase::default_font();
        auto title_font = default_font.bold_variant().with_size(30.0f);
        auto small_font = default_font.with_size(11.0f);

        auto title = "SerenityOS"sv;
        auto subtitle = "Linux GUI Port"sv;

        painter.draw_text(Gfx::FloatRect { 0, 130, static_cast<float>(width()), 40 }, title, *title_font,
            Gfx::TextAlignment::Center, Color::from_rgb(0xffffff));
        painter.draw_text(Gfx::FloatRect { 0, 176, static_cast<float>(width()), 24 }, subtitle, *small_font,
            Gfx::TextAlignment::Center, Color::from_rgb(0xb0c8ff));

        // Segmented progress bar.
        Gfx::IntRect bar_rect { 64, height() - 96, width() - 128, 22 };
        painter.fill_rect(bar_rect, Color::from_rgb(0x000010));
        painter.draw_rect(bar_rect, Color::from_rgb(0x8090c0));

        auto inner = bar_rect.shrunken(3, 3);
        auto segment_gap = 2;
        auto segment_width = (inner.width() - (SEGMENTS - 1) * segment_gap) / SEGMENTS;
        auto filled_segments = (m_progress * SEGMENTS) / 100;
        for (int i = 0; i < SEGMENTS; ++i) {
            Gfx::IntRect segment { inner.x() + i * (segment_width + segment_gap), inner.y(), segment_width, inner.height() };
            if (i < filled_segments)
                painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, segment,
                    Color::from_rgb(0x00c040), Color::from_rgb(0x007020));
            else
                painter.fill_rect(segment, Color::from_rgb(0x181840));
        }

        // Status line and percentage.
        auto percent = ByteString::formatted("{}%", m_progress);
        auto status_rect = Gfx::FloatRect { static_cast<float>(bar_rect.x()), static_cast<float>(bar_rect.bottom() + 8), static_cast<float>(bar_rect.width()), 18 };
        painter.draw_text(status_rect, StringView { m_status }, *small_font,
            Gfx::TextAlignment::CenterLeft, Color::from_rgb(0xd0e0ff));
        painter.draw_text(status_rect, StringView { percent }, *small_font,
            Gfx::TextAlignment::CenterRight, Color::from_rgb(0xd0e0ff));

        (void)full;
    }

    int m_progress { 0 };
    String m_status { "Starting WindowServer..."_string };
};

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("SerenityOS"sv);
    window->set_frameless(true);
    window->set_always_on_top(true);
    window->set_rect({ 192, 184, SPLASH_WIDTH, SPLASH_HEIGHT });
    (void)window->set_main_widget<SplashWidget>();

    auto* splash = static_cast<SplashWidget*>(window->main_widget());
    window->show();

    struct Phase {
        int progress;
        StringView text;
    };
    static Phase const phases[] = {
        { 0, "Starting WindowServer..."sv },
        { 15, "Loading theme and fonts..."sv },
        { 35, "Starting ConfigServer..."sv },
        { 50, "Starting LaunchServer..."sv },
        { 70, "Loading Taskbar..."sv },
        { 88, "Preparing desktop..."sv },
        { 100, "Ready."sv },
    };
    static size_t phase_index = 0;
    static int progress = 0;

    auto timer = Core::Timer::create_repeating(45, [splash] {
        progress = min(progress + 1 + (progress / 25), 100);
        splash->set_progress(progress);

        while (phase_index + 1 < sizeof(phases) / sizeof(phases[0]) && progress >= phases[phase_index + 1].progress)
            ++phase_index;
        splash->set_status(String::from_utf8(phases[phase_index].text).release_value());

        if (progress >= 100)
            Core::EventLoop::current().quit(0);
    });
    timer->start();

    return app->exec();
}
