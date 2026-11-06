/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Video Player for the Linux GUI port.
//
// The upstream player is backed by LibMedia, which only knows VP8/VP9 inside a
// Matroska container -- double-clicking an MP4/AVI/MOV did nothing useful. This
// port instead drives libmpv through its software render API and paints the
// decoded frames into a normal LibGUI VideoFrameWidget. libmpv brings FFmpeg's
// full demuxer/decoder set (H.264, H.265, MPEG-4, VP9, AV1, audio, subtitles)
// without needing a GL/Vulkan context, which this VM does not have.

#include "MpvBackend.h"

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NumberFormat.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/HorizontalSlider.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/SeparatorWidget.h>
#include <LibGUI/Slider.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <unistd.h>

#include "VideoFrameWidget.h"

namespace {

RefPtr<Gfx::Bitmap> load_icon(StringView name)
{
    auto path = ByteString::formatted("resource://icons/16x16/{}.png", name);
    auto bitmap = Gfx::Bitmap::load_from_uri(path.view());
    if (bitmap.is_error())
        return nullptr;
    return bitmap.release_value();
}

String format_time(double seconds)
{
    return human_readable_digital_time(static_cast<i64>(max(0.0, seconds) + 0.5));
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView filename = ""sv;
    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(filename, "The video file to play.", "filename", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title(ByteString { "Video Player" });
    window->resize(820, 600);
    window->set_resizable(true);
    // set_minimum_size() asserts when the window still obeys the widget's min size.
    window->set_obey_widget_min_size(false);
    window->set_minimum_size(400, 300);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>();
    auto* main_layout = static_cast<GUI::VerticalBoxLayout*>(main_widget->layout());
    main_layout->set_margins({ 0 });
    main_layout->set_spacing(0);
    window->set_main_widget(main_widget.ptr());

    auto video_display = VideoPlayer::VideoFrameWidget::construct();
    video_display->set_auto_resize(false);
    // Give the video surface an explicit Grow preferred size and a black
    // autofill. The autofill is important: Widget::handle_paint_event() fills
    // the background *before* paint_event(), so even if the bitmap draw path
    // fails the video area is still black rather than the parent's light
    // background. A collapsed widget (empty rect) never paints at all, which
    // the explicit Grow sizing prevents.
    video_display->set_preferred_size(GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow);
    video_display->set_fill_with_background_color(true);
    video_display->set_background_color(Gfx::Color::Black);
    main_widget->add_child(*video_display);

    auto bottom_container = GUI::Widget::construct();
    bottom_container->set_fixed_height(56);
    bottom_container->set_layout<GUI::VerticalBoxLayout>();
    auto* bottom_layout = static_cast<GUI::VerticalBoxLayout*>(bottom_container->layout());
    bottom_layout->set_margins({ 2 });
    bottom_layout->set_spacing(2);
    main_widget->add_child(*bottom_container);

    auto seek_slider = GUI::HorizontalSlider::construct();
    seek_slider->set_fixed_height(20);
    seek_slider->set_min(0);
    seek_slider->set_max(1000);
    seek_slider->set_enabled(false);
    bottom_container->add_child(*seek_slider);

    auto controls = GUI::Widget::construct();
    controls->set_fixed_height(28);
    controls->set_layout<GUI::HorizontalBoxLayout>();
    auto* controls_layout = static_cast<GUI::HorizontalBoxLayout*>(controls->layout());
    controls_layout->set_margins({ 2 });
    controls_layout->set_spacing(4);
    bottom_container->add_child(*controls);

    auto play_button = GUI::Button::construct();
    play_button->set_fixed_width(30);
    controls->add_child(*play_button);

    auto timestamp_label = GUI::Label::construct();
    timestamp_label->set_fixed_width(140);
    controls->add_child(*timestamp_label);

    controls_layout->add_spacer();

    auto volume_label = GUI::Label::construct();
    volume_label->set_text(MUST(String::from_utf8("Volume:"sv)));
    controls->add_child(*volume_label);

    auto volume_slider = GUI::HorizontalSlider::construct();
    volume_slider->set_fixed_width(100);
    volume_slider->set_min(0);
    volume_slider->set_max(100);
    volume_slider->set_value(100);
    controls->add_child(*volume_slider);

    auto sizing_button = GUI::Button::construct();
    sizing_button->set_fixed_width(30);
    controls->add_child(*sizing_button);

    auto fullscreen_button = GUI::Button::construct();
    fullscreen_button->set_fixed_width(30);
    controls->add_child(*fullscreen_button);

    VideoPlayer::MpvBackend backend;
    ByteString startup_error;
    bool backend_ready = backend.initialize(startup_error);
    if (!backend_ready)
        warnln("VideoPlayer: {}", startup_error);

    auto play_icon = load_icon("play"sv);
    auto pause_icon = load_icon("pause"sv);
    auto sizing_icon = load_icon("fit-image-to-view"sv);
    auto fullscreen_icon = load_icon("fullscreen"sv);

    if (play_icon)
        play_button->set_icon(play_icon);
    if (sizing_icon)
        sizing_button->set_icon(sizing_icon);
    if (fullscreen_icon)
        fullscreen_button->set_icon(fullscreen_icon);

    Function<void()> update_play_pause_icon;
    update_play_pause_icon = [&]() {
        bool playing = backend_ready && backend.has_media() && !backend.is_paused();
        play_button->set_icon(playing ? pause_icon : play_icon);
        play_button->set_enabled(backend_ready && backend.has_media());
    };

    auto toggle_pause = [&]() {
        if (!backend_ready || !backend.has_media())
            return;
        backend.toggle_pause();
        update_play_pause_icon();
    };

    auto toggle_fullscreen = [&]() {
        bool fullscreen = !window->is_fullscreen();
        window->set_fullscreen(fullscreen);
        bottom_container->set_visible(!fullscreen);
        video_display->set_frame_style(fullscreen ? Gfx::FrameStyle::NoFrame : Gfx::FrameStyle::SunkenContainer);
    };

    auto play_pause_action = GUI::Action::create("&Play / Pause", { Key_Space }, play_icon, [&](auto&) {
        toggle_pause();
    });
    play_button->set_action(*play_pause_action);

    auto fullscreen_action = GUI::Action::create("&Fullscreen", { Key_F11 }, fullscreen_icon, [&](auto&) {
        toggle_fullscreen();
    });
    fullscreen_button->set_action(*fullscreen_action);

    Function<void(VideoPlayer::VideoSizingMode)> apply_sizing_mode;
    auto size_fit_action = GUI::Action::create_checkable("&Fit", [&](auto&) { apply_sizing_mode(VideoPlayer::VideoSizingMode::Fit); });
    auto size_fill_action = GUI::Action::create_checkable("Fi&ll", [&](auto&) { apply_sizing_mode(VideoPlayer::VideoSizingMode::Fill); });
    auto size_stretch_action = GUI::Action::create_checkable("&Stretch", [&](auto&) { apply_sizing_mode(VideoPlayer::VideoSizingMode::Stretch); });
    auto size_fullsize_action = GUI::Action::create_checkable("F&ull Size", [&](auto&) { apply_sizing_mode(VideoPlayer::VideoSizingMode::FullSize); });
    apply_sizing_mode = [&](VideoPlayer::VideoSizingMode mode) {
        video_display->set_sizing_mode(mode);
        size_fit_action->set_checked(mode == VideoPlayer::VideoSizingMode::Fit);
        size_fill_action->set_checked(mode == VideoPlayer::VideoSizingMode::Fill);
        size_stretch_action->set_checked(mode == VideoPlayer::VideoSizingMode::Stretch);
        size_fullsize_action->set_checked(mode == VideoPlayer::VideoSizingMode::FullSize);
    };

    auto sizing_action = GUI::Action::create("&Sizing Mode", sizing_icon, [&](auto&) {
        auto mode = static_cast<VideoPlayer::VideoSizingMode>((to_underlying(video_display->sizing_mode()) + 1) % to_underlying(VideoPlayer::VideoSizingMode::Sentinel));
        apply_sizing_mode(mode);
    });
    sizing_button->set_action(*sizing_action);

    video_display->on_click = [&]() { toggle_pause(); };
    video_display->on_doubleclick = [&]() { toggle_fullscreen(); };

    auto show_error = [&](StringView text) {
        GUI::MessageBox::show_error(window.ptr(), text);
    };

    auto open_file = [&](ByteString const& path) {
        if (!backend_ready) {
            show_error(startup_error.view());
            return;
        }
        if (access(path.characters(), R_OK) != 0) {
            auto message = ByteString::formatted("Cannot read '{}'", path);
            show_error(message.view());
            return;
        }
        ByteString error;
        if (!backend.load_file(path, error)) {
            show_error(error.view());
            return;
        }
        window->set_title(ByteString::formatted("{} - Video Player", path));
        seek_slider->set_enabled(true);
        update_play_pause_icon();
    };

    auto choose_file = [&]() {
        // GUI::FilePicker goes through FileSystemAccessClient, whose the() aborts
        // (VERIFY on an Error) when the FileSystemAccessServer portal socket is
        // missing -- e.g. the service crashed or was never started. Probe the
        // socket first so a dead service does not take the player down with it.
        auto portal_path = Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/filesystemaccess"sv);
        if (portal_path.is_error() || access(portal_path.value().characters(), F_OK) != 0) {
            show_error("The File System Access service is not running, so Open dialogs are unavailable.\n"
                       "Start a video from the File Manager (double-click) or pass its path on the command line."sv);
            return;
        }
        Vector<GUI::FileTypeFilter> filters;
        filters.append(GUI::FileTypeFilter::video_files());
        filters.append(GUI::FileTypeFilter::all_files());
        auto path = GUI::FilePicker::get_open_filepath(window.ptr(), "Open Video", Core::StandardPaths::home_directory(), false, GUI::Dialog::ScreenPosition::CenterWithinParent, Optional<Vector<GUI::FileTypeFilter>> { move(filters) });
        if (path.has_value())
            open_file(path.value());
    };

    // ---- Menus ----
    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::Action::create("&Open...", { Mod_Ctrl, Key_O }, load_icon("open"sv), [&](auto&) { choose_file(); }));
    file_menu->add_separator();
    file_menu->add_action(GUI::CommonActions::make_quit_action([&](auto&) { window->close(); }));

    auto playback_menu = window->add_menu("&Playback"_string);
    playback_menu->add_action(*play_pause_action);
    playback_menu->add_separator();
    playback_menu->add_action(GUI::Action::create("Seek &Back 10s", { Key_Left }, [&](auto&) {
        if (backend_ready && backend.has_media())
            backend.seek_seconds(max(0.0, backend.position() - 10));
    }));
    playback_menu->add_action(GUI::Action::create("Seek &Forward 10s", { Key_Right }, [&](auto&) {
        if (backend_ready && backend.has_media())
            backend.seek_seconds(backend.position() + 10);
    }));

    auto view_menu = window->add_menu("&View"_string);
    view_menu->add_action(*fullscreen_action);
    auto sizing_submenu = view_menu->add_submenu("&Sizing Mode"_string);
    auto sizing_group = make<GUI::ActionGroup>();
    sizing_group->set_exclusive(true);
    sizing_group->add_action(*size_fit_action);
    sizing_group->add_action(*size_fill_action);
    sizing_group->add_action(*size_stretch_action);
    sizing_group->add_action(*size_fullsize_action);
    sizing_submenu->add_action(*size_fit_action);
    sizing_submenu->add_action(*size_fill_action);
    sizing_submenu->add_action(*size_stretch_action);
    sizing_submenu->add_action(*size_fullsize_action);

    apply_sizing_mode(VideoPlayer::VideoSizingMode::Fit);

    // ---- Slider callbacks ----
    bool was_playing_before_seek = false;
    seek_slider->on_drag_start = [&]() {
        was_playing_before_seek = backend_ready && backend.has_media() && !backend.is_paused();
        if (was_playing_before_seek)
            backend.set_paused(true);
    };
    seek_slider->on_drag_end = [&]() {
        if (was_playing_before_seek) {
            was_playing_before_seek = false;
            backend.set_paused(false);
        }
        update_play_pause_icon();
    };
    seek_slider->on_change = [&](int value) {
        if (!backend_ready || !backend.has_media())
            return;
        double duration = backend.duration();
        if (duration <= 0)
            return;
        backend.seek_seconds(value / 1000.0 * duration);
    };

    volume_slider->on_change = [&](int value) {
        if (backend_ready)
            backend.set_volume(value);
    };

    // ---- Frame pump ----
    // mpv's update callback only flips an atomic; the GUI thread does the
    // actual render + repaint. render_frame() is non-blocking (we pass
    // BLOCK_FOR_TARGET_TIME=0), so this never stalls the event loop.
    auto timer = Core::Timer::create_repeating(16, [&]() {
        if (!backend_ready)
            return;

        // Diagnostics: opt-in via SERENITY_VP_DEBUG=1 (throttled to ~once per
        // second). "Audio but no video" is otherwise impossible to debug over
        // SSH on this VM, but the log spam is not wanted by default.
        static bool const vp_debug = getenv("SERENITY_VP_DEBUG") != nullptr;
        static int diag_tick = 0;
        static int render_ok = 0;
        static int render_null = 0;
        static bool dumped_first_frame = false;
        ++diag_tick;

        if (backend.frame_pending()) {
            // Render at the video's own (capped) size. Do NOT shrink the render
            // surface to the widget size: mpv's software renderer produced a
            // flat black surface when the requested SW size did not match the
            // video's native aspect/size, so LibGUI's Fit blit is left to do
            // the downscale.
            Gfx::IntSize size = backend.video_size();
            if (size.width() <= 0 || size.height() <= 0) {
                auto rect = video_display->frame_inner_rect();
                size = { max(16, rect.width()), max(16, rect.height()) };
            }
            if (auto const* bitmap = backend.render_frame(size.width(), size.height())) {
                ++render_ok;
                video_display->set_bitmap(bitmap);
                video_display->update();

                // Dump the very first decoded frame as a PPM. This proves (or
                // disproves) that the libmpv SW surface contains real pixels,
                // independently of whether LibGUI manages to paint it.
                if (vp_debug && !dumped_first_frame) {
                    dumped_first_frame = true;
                    if (FILE* f = fopen("/tmp/video-player-first-frame.ppm", "wb")) {
                        fprintf(f, "P6\n%d %d\n255\n", bitmap->width(), bitmap->height());
                        for (int y = 0; y < bitmap->height(); ++y) {
                            for (int x = 0; x < bitmap->width(); ++x) {
                                auto c = bitmap->get_pixel(x, y);
                                unsigned char rgb[3] = { c.red(), c.green(), c.blue() };
                                fwrite(rgb, 1, 3, f);
                            }
                        }
                        fclose(f);
                        dbgln("VPDIAG: dumped first frame to /tmp/video-player-first-frame.ppm");
                    }
                }
            } else {
                ++render_null;
            }
        }

        if (vp_debug && diag_tick % 60 == 0) {
            auto ws = video_display->size();
            auto inner = video_display->frame_inner_rect();
            auto const* bm = video_display->bitmap();
            auto vs = backend.video_size();
            u64 pixsum = 0;
            if (bm) {
                for (int i = 0; i < 8; ++i) {
                    int x = (i * 37 + 11) % max(1, bm->width());
                    int y = (i * 53 + 7) % max(1, bm->height());
                    auto c = bm->get_pixel(x, y);
                    pixsum += static_cast<u64>(c.red()) + c.green() + c.blue();
                }
            }
            dbgln("VPDIAG tick={} render_ok={} render_null={} widget={}x{} inner={}x{} visible={} window={}x{} bitmap={}x{} pixsum={} vsize={}x{} pending={}",
                diag_tick, render_ok, render_null,
                ws.width(), ws.height(), inner.width(), inner.height(),
                video_display->is_visible(),
                window->size().width(), window->size().height(),
                bm ? bm->width() : 0, bm ? bm->height() : 0,
                pixsum, vs.width(), vs.height(), backend.frame_pending());
        }

        if (!backend.has_media())
            return;

        double duration = backend.duration();
        double position = backend.position();

        StringBuilder builder;
        builder.append(format_time(position));
        builder.append(" / "sv);
        if (duration > 0)
            builder.append(format_time(duration));
        else
            builder.append("--:--:--"sv);
        timestamp_label->set_text(MUST(builder.to_string()));

        if (!seek_slider->knob_dragging() && duration > 0)
            seek_slider->set_value(static_cast<int>(position / duration * 1000), GUI::AllowCallback::No);

        bool playing = !backend.is_paused();
        auto expected_icon = playing ? pause_icon.ptr() : play_icon.ptr();
        if (play_button->icon() != expected_icon)
            update_play_pause_icon();
    });
    timer->start();

    auto const app_icon = GUI::Icon::default_icon("app-video-player"sv);
    window->set_icon(app_icon.bitmap_for_size(16));

    window->show();

    if (!backend_ready) {
        auto message = ByteString::formatted("{}\n\nPlayback needs the libmpv2 package on this system.", startup_error);
        GUI::MessageBox::show(window.ptr(), message.view(), "Video Player"sv);
    } else if (!filename.is_empty()) {
        open_file(ByteString { filename });
    } else {
        update_play_pause_icon();
        timestamp_label->set_text(MUST(String::from_utf8("--:--:-- / --:--:--"sv)));
    }

    return app->exec();
}
