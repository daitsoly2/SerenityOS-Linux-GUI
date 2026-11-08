/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Volume tray applet for the Linux GUI port.
//
// Why this exists: the upstream Audio applet talks to AudioServer over IPC via
// LibAudio, but LibAudio's ConnectionTo{Server,ManagerServer} are only compiled
// on SerenityOS, and there is no AudioServer on a Linux host. This applet keeps
// the same UI/behaviour but drives the host mixer instead (amixer/pactl/wpctl),
// and paints an icon whose look follows the level.

#include <AK/Vector.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/Process.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Application.h>
#include <LibGUI/Button.h>
#include <LibGUI/Event.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Slider.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Label.h>
#include <LibGUI/Painter.h>
#include <LibGUI/HorizontalSlider.h>
#include <LibGUI/UITranslation.h>
#include <LibGUI/Widget.h>
#include <LibGfx/StylePainter.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <Ports/LinuxGUI/Common/Translation.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

char const* CONFIG_PATH = "/etc/serenity/volume.ini";

int read_volume()
{
    auto config = Core::ConfigFile::open(ByteString { CONFIG_PATH });
    if (config.is_error())
        return 60;
    auto value = config.value()->read_num_entry("Volume", "Level", 60);
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    return value;
}

void apply_volume(int level)
{
    auto command = ByteString::formatted("amixer -q sset Master {}% 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ {}% 2>/dev/null", level, level);
    if (auto result = Core::Process::spawn("/bin/sh"sv, Array<StringView, 2> { "-c"sv, command.view() }); result.is_error())
        warnln("volume: mixer command failed: {}", result.error());
}

void store_volume(int level)
{
    MUST(Core::Directory::create(LexicalPath::dirname(ByteString { CONFIG_PATH }), Core::Directory::CreateDirectories::Yes));
    auto config = Core::ConfigFile::open(ByteString { CONFIG_PATH }, Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error())
        return;
    config.value()->write_num_entry("Volume", "Level", level);
    (void)config.value()->sync();
}

class VolumeWidget final : public GUI::Widget {
    C_OBJECT(VolumeWidget)

public:
    VolumeWidget() = default;
    virtual ~VolumeWidget() override = default;

    void set_level(int level)
    {
        m_level = level;
        update();
    }

    Function<void()> on_click;

private:
    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() == GUI::MouseButton::Primary && on_click)
            on_click();
        GUI::Widget::mousedown_event(event);
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);

        auto icon_path = ByteString { "/res/icons/16x16/audio-volume-high.png" };
        if (m_level == 0)
            icon_path = ByteString { "/res/icons/16x16/audio-volume-muted.png" };
        else if (m_level < 40)
            icon_path = ByteString { "/res/icons/16x16/audio-volume-low.png" };
        else if (m_level < 75)
            icon_path = ByteString { "/res/icons/16x16/audio-volume-medium.png" };

        if (auto bitmap = Gfx::Bitmap::load_from_file(icon_path, 1); !bitmap.is_error()) {
            auto& icon = *bitmap.value();
            auto icon_rect = Gfx::IntRect { 3, (height() - icon.height()) / 2, icon.width(), icon.height() };
            painter.draw_scaled_bitmap(icon_rect, icon, icon.rect());
        }
    }

    int m_level { 60 };
};

class VolumePopup final : public GUI::Window {
    C_OBJECT(VolumePopup)

public:
    static NonnullRefPtr<VolumePopup> construct(int level)
    {
        auto window = adopt_ref(*new VolumePopup);
        window->build(level);
        return window;
    }

private:
    void build(int level)
    {
        // Win9x 风格：下沉面板 + 竖滑块 + 百分比。用普通窗口（带关闭按钮），
        // 因为 Popup/无边框窗口在这套 WindowServer 里没有定位和关闭的入口。
        // 但它是从托盘弹出的临时面板，不是应用主窗口：不占任务栏按钮。
        set_title(GUI::translate_ui("Volume"_string).to_byte_string());
        set_skip_taskbar(true);
        // A transient tray flyout needs no title bar: it is not a taskbar
        // window, it is positioned by --position, and it closes by clicking
        // the tray button again, the x, or anywhere else.
        set_frameless(true);
        set_resizable(false);
        resize(96, 240);

        auto widget = GUI::Frame::construct();
        // No window frame at all: this is a transient flyout, not a dialog.
        // A plain flat panel plus the palette background is enough to separate
        // it from the desktop; the 3D sunken border made it look like another
        // application window.
        widget->set_frame_style(Gfx::FrameStyle::NoFrame);
        widget->set_fill_with_background_color(true);
        set_main_widget(widget);
        widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8, 8, 8, 8 }, 4);

        auto& header = widget->add<GUI::Widget>();
        header.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 2);
        header.set_fixed_height(16);
        auto& caption = header.add<GUI::Label>();
        caption.set_text("音量"_string);
        auto& close_button = header.add<GUI::Button>();
        close_button.set_text("x"_string);
        close_button.set_button_style(Gfx::ButtonStyle::Coolbar);
        close_button.set_fixed_size(18, 16);
        close_button.set_tooltip("关闭"_string);
        close_button.on_click = [this](unsigned) { close(); };

        // Losing focus (a click anywhere else) closes the flyout. Only react
        // after we have actually been active, so the initial inactive state
        // does not close the window before it is shown.
        bool was_active = false;
        on_active_window_change = [this, was_active](bool active) mutable {
            if (active) {
                was_active = true;
                return;
            }
            if (was_active)
                close();
        };

        auto& slider = widget->add<GUI::Slider>();
        slider.set_orientation(Orientation::Vertical);
        slider.set_range(0, 100);
        slider.set_value(level);

        auto& label = widget->add<GUI::Label>();
        label.set_text_alignment(Gfx::TextAlignment::Center);
        label.set_text(MUST(String::formatted("{}%", level)));

        slider.on_change = [label = &label](int value) {
            apply_volume(value);
            store_volume(value);
            label->set_text(MUST(String::formatted("{}%", value)));
        };
    }
};

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    // --window: 直接开一个独立音量窗口（任务栏托盘按钮用这个）。
    // --position x,y 让窗口贴着点击的托盘按钮弹出，而不是由 WindowServer
    // 随便放到屏幕角落。
    bool open_window = false;
    String position_text;
    Core::ArgsParser parser;
    parser.add_option(open_window, "Open a standalone volume window instead of the tray applet", "window", 'w');
    parser.add_option(position_text, "Place the standalone window at X,Y", "position", 'p', "x,y");
    parser.parse(arguments);

    auto level = read_volume();

    if (open_window) {
        // One popup at a time. Clicking the tray button while the panel is
        // already open closes it instead of spawning a second process (and a
        // second window / taskbar button on every click).
        constexpr char const* lock_path = "/tmp/.serenity-volume-popup.lock";
        int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            char pid_text[32] {};
            if (lock_fd >= 0) {
                lseek(lock_fd, 0, SEEK_SET);
                (void)!read(lock_fd, pid_text, sizeof(pid_text) - 1);
            }
            int existing_pid = atoi(pid_text);
            if (existing_pid > 1)
                kill(existing_pid, SIGTERM);
            if (lock_fd >= 0)
                close(lock_fd);
            return 0;
        }
        // Keep lock_fd open: the kernel drops the flock when this process dies,
        // which is exactly when the panel is closed.
        ftruncate(lock_fd, 0);
        char pid_text[32] {};
        int length = snprintf(pid_text, sizeof(pid_text), "%d\n", getpid());
        (void)!write(lock_fd, pid_text, static_cast<size_t>(length));

        auto window = VolumePopup::construct(level);
        if (!position_text.is_empty()) {
            auto parts = TRY(position_text.split(','));
            if (parts.size() == 2) {
                auto x = parts[0].to_number<int>();
                auto y = parts[1].to_number<int>();
                if (x.has_value() && y.has_value())
                    window->move_to(x.value(), y.value());
            }
        }
        window->show();
        window->move_to_front();
        return app->exec();
    }

    auto window = GUI::Window::construct();
    window->set_title("Volume"sv);
    window->set_window_type(GUI::WindowType::Applet);
    window->set_frameless(true);
    window->set_resizable(false);
    window->resize(24, 22);
    auto widget = VolumeWidget::construct();
    widget->set_level(level);
    window->set_main_widget(widget);
    window->show();

    auto popup = VolumePopup::construct(level);

    widget->on_click = [&] {
        auto position = window->position();
        popup->move_to(position.x() - 78, position.y() - 64);
        popup->show();
        popup->move_to_front();
    };

    return app->exec();
}
