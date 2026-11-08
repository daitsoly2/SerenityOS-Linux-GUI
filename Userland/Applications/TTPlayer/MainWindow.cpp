/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MainWindow.h"
#include "EqualizerWindow.h"
#include "LyricsWindow.h"
#include "PlaylistWindow.h"
#include "SkinPanel.h"
#include "SkinText.h"
#include "SkinTheme.h"
#include "TTSkin.h"
#include <AK/NumberFormat.h>
#include <LibConfig/Client.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/Icon.h>
#include <LibGUI/MessageBox.h>
#include <LibCore/StandardPaths.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Event.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Painter.h>

namespace {

constexpr StringView config_domain = "TTPlayer"sv;

constexpr int main_window_width = 344;
constexpr int main_window_height = 176;

ByteString state_text_for(PlayerCore::State state)
{
    switch (state) {
    case PlayerCore::State::Playing:
        return ByteString { "状态: 播放" };
    case PlayerCore::State::Paused:
        return ByteString { "状态: 暂停" };
    case PlayerCore::State::Stopped:
        return ByteString { "状态: 停止" };
    case PlayerCore::State::NoFileLoaded:
        return ByteString { "状态: 待机" };
    }
    return ByteString { "状态: 待机" };
}

}

// ---------------------------------------------------------------------------
// TrackInfoBox
// ---------------------------------------------------------------------------

void TrackInfoBox::set_info(ByteString title, ByteString artist, ByteString album)
{
    if (m_title == title && m_artist == artist && m_album == album)
        return;
    m_title = move(title);
    m_artist = move(artist);
    m_album = move(album);
    update();
}

void TrackInfoBox::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto inner = rect();
    TTSkin::paint_inset(painter, inner);
    inner.shrink(6, 3);

    if (m_title.is_empty()) {
        painter.draw_text(inner, "千千静听 · 音乐播放器"sv, font().bold_variant(),
            Gfx::TextAlignment::CenterLeft, TTSkin::colors().faint_text);
        painter.draw_text(Gfx::IntRect { inner.x(), inner.y() + 18, inner.width(), 16 },
            "把音乐拖进窗口即可播放"sv, font(), Gfx::TextAlignment::CenterLeft, TTSkin::colors().faint_text);
        return;
    }

    int line_height = 14;
    painter.draw_text(Gfx::IntRect { inner.x(), inner.y(), inner.width(), line_height }, m_title,
        font().bold_variant(), Gfx::TextAlignment::CenterLeft, TTSkin::colors().inset_text);

    auto artist_text = m_artist.is_empty() ? ByteString { "艺术家: 未知" } : ByteString::formatted("艺术家: {}", m_artist);
    painter.draw_text(Gfx::IntRect { inner.x(), inner.y() + line_height, inner.width(), line_height }, artist_text,
        font(), Gfx::TextAlignment::CenterLeft, TTSkin::colors().inset_dim_text);

    auto album_text = m_album.is_empty() ? ByteString { "专辑: 未知" } : ByteString::formatted("专辑: {}", m_album);
    painter.draw_text(Gfx::IntRect { inner.x(), inner.y() + 2 * line_height, inner.width(), line_height }, album_text,
        font(), Gfx::TextAlignment::CenterLeft, TTSkin::colors().faint_text);
}

// ---------------------------------------------------------------------------
// StatusPanel
// ---------------------------------------------------------------------------

void StatusPanel::set_state_text(ByteString text)
{
    if (m_state == text)
        return;
    m_state = move(text);
    update();
}

void StatusPanel::set_format_text(ByteString text)
{
    if (m_format == text)
        return;
    m_format = move(text);
    update();
}

void StatusPanel::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    int line_height = height() / 2;
    painter.draw_text(Gfx::IntRect { 0, 0, width(), line_height }, m_format, font(),
        Gfx::TextAlignment::CenterLeft, TTSkin::colors().accent);
    painter.draw_text(Gfx::IntRect { 0, line_height, width(), line_height }, m_state, font(),
        Gfx::TextAlignment::CenterLeft, TTSkin::colors().dim_text);
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(NonnullRefPtr<PlayerCore> core)
    : m_core(move(core))
{
    set_title(ByteString { "千千静听" });
    set_icon(GUI::Icon::default_icon("app-ttplayer"sv).bitmap_for_size(16));
    set_obey_widget_min_size(false);
    set_resizable(false);
    // 千千静听 never shows an OS title bar: the skin draws its own strip.
    set_frameless(true);

    auto screen = GUI::Desktop::the().rect();
    auto fallback_size = Gfx::IntSize { main_window_width, main_window_height };
    auto fallback_position = Gfx::IntPoint { 64, max(32, screen.height() / 3) };
    restore_size_and_position(config_domain, "MainWindow"sv, fallback_size, fallback_position);
    if (width() < 280 || height() < 120)
        resize(fallback_size);
    save_size_and_position_on_close(config_domain, "MainWindow"sv);

    build_ui();
    build_menus();
    wire_core();
    sync_from_core();

    start_sticky_watch();

    m_always_on_top = Config::read_bool(config_domain, "Preferences"sv, "AlwaysOnTop"sv, false);
    set_all_windows_on_top(m_always_on_top);

    if (Config::read_bool(config_domain, "Preferences"sv, "PlaylistWindowVisible"sv, true))
        set_playlist_window_visible(true);
    if (Config::read_bool(config_domain, "Preferences"sv, "LyricsWindowVisible"sv, true))
        set_lyrics_window_visible(true);
    if (Config::read_bool(config_domain, "Preferences"sv, "EqualizerWindowVisible"sv, false))
        set_equalizer_window_visible(true);

    apply_window_shape();
    apply_skin_geometry();
}

MainWindow::~MainWindow() = default;

NonnullRefPtr<MainWindow> MainWindow::create(NonnullRefPtr<PlayerCore> core)
{
    return adopt_ref(*new MainWindow(move(core)));
}

void MainWindow::play_file_path(ByteString const& path)
{
    m_core->play_file_path(path);
}

void MainWindow::build_ui()
{
    auto root_widget = set_main_widget<SkinPanel>();
    root_widget->set_kind(SkinTheme::WindowKind::Main);
    root_widget->on_close_clicked = [] { GUI::Application::the()->quit(); };
    root_widget->on_context_menu_request = [this](auto& event) { show_context_menu(event); };
    auto& root = *root_widget;

    // Native Skin.xml control names -> playback actions. Classic skins never
    // create a canvas, so these hooks are simply unused in that mode.
    root_widget->on_native_action = [this](StringView name) {
        if (name == "play"sv || name == "pause"sv)
            m_core->toggle_play_pause();
        else if (name == "stop"sv)
            m_core->stop_playback();
        else if (name == "next"sv)
            m_core->next_track();
        else if (name == "prev"sv)
            m_core->previous_track();
        else if (name == "open"sv)
            m_core->add_files(this);
        else if (name == "mute"sv)
            m_core->set_muted(!m_core->muted());
        else if (name == "exit"sv)
            GUI::Application::the()->quit();
        else if (name == "minimize"sv)
            set_minimized(true);
        else if (name == "playlist"sv)
            toggle_playlist_window();
        else if (name == "lyric"sv)
            toggle_lyrics_window();
        else if (name == "equalizer"sv)
            toggle_equalizer_window();
        else if (name.starts_with("mode_"sv))
            m_core->cycle_play_mode();
    };
    root_widget->on_native_slider = [this](StringView name, float ratio) {
        if (name == "progress"sv)
            m_core->seek_to_ms(static_cast<int>(ratio * m_core->duration_ms()));
        else if (name == "volume"sv)
            m_core->set_volume(ratio);
    };
    root_widget->on_native_canvas_setup = [this](SkinCanvas&) {
        update_native_canvas();
    };

    root.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14, 6, 6 }, 4);

    // ---- Row 1: song info + LED clock/state ------------------------------
    auto& top_row = root.add<GUI::Widget>();
    top_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 5);
    top_row.set_fixed_height(58);

    m_info_box = &top_row.add<TrackInfoBox>();

    auto& side_column = top_row.add<GUI::Widget>();
    side_column.set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, 3);
    side_column.set_fixed_width(106);
    m_led = &side_column.add<LedDisplay>();
    m_status = &side_column.add<StatusPanel>();
    m_status->set_fixed_height(22);

    // ---- Row 2: marquee title -------------------------------------------
    m_marquee = &root.add<MarqueeLabel>();
    m_marquee->set_fixed_height(18);
    m_marquee->set_text("千千静听 · 请添加音乐"sv);

    // ---- Row 3: seek bar -------------------------------------------------
    m_seek = &root.add<FlatSlider>();
    m_seek->set_range(0, 1000);
    m_seek->set_fixed_height(14);
    m_seek->on_drag_end = [this](int value) {
        m_core->seek_to_ms(value);
    };
    m_seek->on_change = [this](int value) {
        // Live preview of the target position while dragging.
        if (m_seek->is_dragging())
            m_led->set_times(value / 1000, m_core->duration_ms() / 1000);
    };

    // ---- Row 4: transport buttons ---------------------------------------
    auto& transport_row = root.add<GUI::Widget>();
    transport_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 5);
    transport_row.set_fixed_height(26);

    m_transport = &transport_row.add<TransportBar>();
    m_transport->on_previous = [this] { m_core->previous_track(); };
    m_transport->on_play = [this] { m_core->toggle_play_pause(); };
    m_transport->on_pause = [this] {
        if (m_core->is_playing())
            m_core->toggle_play_pause();
    };
    m_transport->on_stop = [this] { m_core->stop_playback(); };
    m_transport->on_next = [this] { m_core->next_track(); };
    m_transport->on_eject = [this] { m_core->add_files(this); };
    transport_row.add_spacer();

    // ---- Row 5: window toggles + volume ---------------------------------
    auto& toggle_row = root.add<GUI::Widget>();
    toggle_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 4);
    toggle_row.set_fixed_height(20);

    auto add_toggle = [&](String text, int width, Function<void()> on_click) {
        auto& button = toggle_row.add<SkinButton>();
        button.set_text(text.to_byte_string());
        button.set_checkable(true);
        button.set_fixed_size(width, 18);
        button.on_click = move(on_click);
        return &button;
    };

    m_playlist_toggle = add_toggle("列表"_string, 40, [this] { set_playlist_window_visible(m_playlist_toggle->is_checked()); });
    m_lyrics_toggle = add_toggle("歌词"_string, 40, [this] { set_lyrics_window_visible(m_lyrics_toggle->is_checked()); });
    m_eq_toggle = add_toggle("均衡"_string, 40, [this] { set_equalizer_window_visible(m_eq_toggle->is_checked()); });

    toggle_row.add_spacer();

    auto& volume_label = toggle_row.add<SkinText>(SkinText::Role::Dim, 18);
    volume_label.set_text("音量"sv);
    volume_label.set_fixed_width(30);

    m_volume = &toggle_row.add<FlatSlider>();
    m_volume->set_fixed_width(88);
    m_volume->set_range(0, 100);
    m_volume->set_value(static_cast<int>(m_core->volume() * 100));
    m_volume->on_change = [this](int value) { m_core->set_volume(value / 100.0); };
    // Persist once the drag ends: writing the config on every pixel would spam
    // ConfigServer over IPC and can starve the audio feed.
    m_volume->on_drag_end = [this](int) { m_core->persist_volume(); };

    // ---- Spectrum analyzer ----------------------------------------------
    // Classic skins bake the <visual> box into the backdrop but leave the
    // bars to the player; the LibGUI layout has no slot for it either, so it
    // stays hidden until a native canvas claims that element.
    m_spectrum = SpectrumWidget::construct();
    m_spectrum->set_samplerate(m_core->sample_rate());
    // Added outside the vertical box layout on purpose: the spectrum only
    // exists on top of a native skin's <visual> box and must not reserve a
    // row in the classic LibGUI layout.
    root.add_child(*m_spectrum);
    root_widget->set_content_widget(*m_spectrum, "visual"sv, SkinPanel::ContentFill::ExactBounds, false);
    m_core->on_audio_buffer = [this](FixedArray<Audio::Sample> const& buffer) {
        if (m_spectrum)
            m_spectrum->set_buffer(buffer);
    };
}

void MainWindow::build_menus()
{
    auto file_menu = add_menu("文件(&F)"_string);
    file_menu->add_action(GUI::Action::create("打开文件…", { Mod_Ctrl, Key_O }, [this](auto&) { m_core->add_files(this); }));
    file_menu->add_action(GUI::Action::create("添加目录…", { Mod_Ctrl, Key_D }, [this](auto&) { m_core->add_folder(this); }));
    file_menu->add_separator();
    file_menu->add_action(GUI::Action::create("打开播放列表…", { Mod_Ctrl, Key_L }, [this](auto&) { m_core->open_playlist(this); }));
    file_menu->add_action(GUI::Action::create("保存播放列表…", { Mod_Ctrl, Key_S }, [this](auto&) { m_core->save_playlist(this); }));
    file_menu->add_separator();
    file_menu->add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit();
    }));

    auto playback_menu = add_menu("播放(&P)"_string);
    playback_menu->add_action(GUI::Action::create("播放 / 暂停", { Key_Space }, [this](auto&) { m_core->toggle_play_pause(); }));
    playback_menu->add_action(GUI::Action::create("停止", { Key_S }, [this](auto&) { m_core->stop_playback(); }));
    playback_menu->add_separator();
    playback_menu->add_action(GUI::Action::create("上一首", { Mod_Ctrl, Key_Left }, [this](auto&) { m_core->previous_track(); }));
    playback_menu->add_action(GUI::Action::create("下一首", { Mod_Ctrl, Key_Right }, [this](auto&) { m_core->next_track(); }));
    playback_menu->add_separator();

    m_mode_actions.set_exclusive(true);
    auto make_mode_action = [&](ByteString label, PlaylistStore::PlayMode mode) {
        auto action = GUI::Action::create_checkable(move(label), [this, mode](auto&) { m_core->set_play_mode(mode); });
        m_mode_actions.add_action(*action);
        playback_menu->add_action(action);
        if (m_core->play_mode() == mode)
            action->set_checked(true);
    };
    make_mode_action(ByteString { "顺序播放" }, PlaylistStore::PlayMode::Sequential);
    make_mode_action(ByteString { "列表循环" }, PlaylistStore::PlayMode::RepeatAll);
    make_mode_action(ByteString { "单曲循环" }, PlaylistStore::PlayMode::RepeatOne);
    make_mode_action(ByteString { "随机播放" }, PlaylistStore::PlayMode::Shuffle);

    auto view_menu = add_menu("查看(&V)"_string);
    auto playlist_action = GUI::Action::create_checkable("播放列表", { Mod_Ctrl, Key_B }, [this](auto&) {
        set_playlist_window_visible(!playlist_window_visible());
    });
    view_menu->add_action(playlist_action);
    m_playlist_window_action = playlist_action;

    auto lyrics_action = GUI::Action::create_checkable("歌词秀", { Mod_Ctrl, Key_Y }, [this](auto&) {
        set_lyrics_window_visible(!lyrics_window_visible());
    });
    view_menu->add_action(lyrics_action);
    m_lyrics_window_action = lyrics_action;

    auto equalizer_action = GUI::Action::create_checkable("均衡器", { Mod_Ctrl, Key_E }, [this](auto&) {
        set_equalizer_window_visible(!equalizer_window_visible());
    });
    view_menu->add_action(equalizer_action);
    m_equalizer_window_action = equalizer_action;

    build_skin_menu(*view_menu);

    auto help_menu = add_menu("帮助(&H)"_string);
    help_menu->add_action(GUI::CommonActions::make_about_action("千千静听"_string,
        GUI::Icon::default_icon("app-ttplayer"sv), this));
}

namespace {

void repaint_tree(GUI::Widget& widget)
{
    widget.update();
    widget.for_each_child_widget([](GUI::Widget& child) {
        repaint_tree(child);
        return IterationDecision::Continue;
    });
}

}

void MainWindow::build_skin_menu(GUI::Menu& view_menu)
{
    SkinTheme::the().rescan();

    auto skin_menu = view_menu.add_submenu("皮肤(&K)"_string);
    m_skin_actions.set_exclusive(true);

    auto add_skin_entry = [&](ByteString name, int index) {
        auto action = GUI::Action::create_checkable(move(name), [this, index](auto&) { apply_skin(index); });
        m_skin_actions.add_action(*action);
        skin_menu->add_action(action);
        if (SkinTheme::the().current_index() == index)
            action->set_checked(true);
    };

    add_skin_entry(ByteString { "内置经典" }, -1);
    for (int i = 0; i < SkinTheme::the().count(); i++)
        add_skin_entry(SkinTheme::the().name_at(i), i);

    skin_menu->add_separator();
    skin_menu->add_action(GUI::Action::create(ByteString { "从文件安装皮肤…" }, [this](auto&) { install_skin_package(); }));
}

void MainWindow::apply_skin(int index)
{
    SkinTheme::the().select(index);
    apply_window_shape();
    apply_skin_geometry();
    if (auto* panel = static_cast<SkinPanel*>(main_widget()))
        panel->refresh_native_layout();
    repaint_all();
}

void MainWindow::install_skin_package()
{
    auto maybe_path = GUI::FilePicker::get_open_filepath(this, "选择皮肤包(.tsk/.msk/.zip)",
        Core::StandardPaths::home_directory(), /*folder=*/false,
        GUI::Dialog::ScreenPosition::CenterWithinParent);
    if (!maybe_path.has_value())
        return;

    auto result = SkinTheme::the().install_package(*maybe_path);
    if (result.is_error()) {
        GUI::MessageBox::show(this, ByteString::formatted("安装皮肤失败：{}", result.error()), "千千静听"sv,
            GUI::MessageBox::Type::Error);
        return;
    }
    SkinTheme::the().rescan();
    GUI::MessageBox::show(this, ByteString::formatted("皮肤「{}」已安装，可在右键菜单的「皮肤」里选择。", result.value()),
        "千千静听"sv, GUI::MessageBox::Type::Information);
}

void MainWindow::show_context_menu(GUI::ContextMenuEvent const& event)
{
    build_context_menu();
    m_context_menu->popup(event.screen_position());
}

void MainWindow::build_context_menu()
{
    // Frameless windows have no menu bar, so this popup *is* the player's main
    // menu (right-click anywhere on a skin, or press the Menu key). Rebuild it
    // on every popup: skins can be installed at runtime and every check mark
    // has to follow the live player state, without keeping a second tree of
    // actions in sync with the (hidden) system menu bar.
    m_context_menu = GUI::Menu::construct();
    auto& menu = *m_context_menu;

    menu.add_action(GUI::Action::create(ByteString { "打开文件…" }, [this](auto&) { m_core->add_files(this); }));
    menu.add_action(GUI::Action::create(ByteString { "添加目录…" }, [this](auto&) { m_core->add_folder(this); }));
    menu.add_action(GUI::Action::create(ByteString { "打开播放列表…" }, [this](auto&) { m_core->open_playlist(this); }));
    menu.add_action(GUI::Action::create(ByteString { "保存播放列表…" }, [this](auto&) { m_core->save_playlist(this); }));
    menu.add_separator();

    menu.add_action(GUI::Action::create(ByteString { "播放 / 暂停" }, [this](auto&) { m_core->toggle_play_pause(); }));
    menu.add_action(GUI::Action::create(ByteString { "停止" }, [this](auto&) { m_core->stop_playback(); }));
    menu.add_action(GUI::Action::create(ByteString { "上一首" }, [this](auto&) { m_core->previous_track(); }));
    menu.add_action(GUI::Action::create(ByteString { "下一首" }, [this](auto&) { m_core->next_track(); }));
    menu.add_separator();

    auto mode_menu = menu.add_submenu("播放模式"_string);
    auto add_mode = [&](ByteString label, PlaylistStore::PlayMode mode) {
        auto action = GUI::Action::create_checkable(move(label), [this, mode](auto&) { m_core->set_play_mode(mode); });
        action->set_checked(m_core->play_mode() == mode);
        mode_menu->add_action(action);
    };
    add_mode(ByteString { "顺序播放" }, PlaylistStore::PlayMode::Sequential);
    add_mode(ByteString { "列表循环" }, PlaylistStore::PlayMode::RepeatAll);
    add_mode(ByteString { "单曲循环" }, PlaylistStore::PlayMode::RepeatOne);
    add_mode(ByteString { "随机播放" }, PlaylistStore::PlayMode::Shuffle);
    menu.add_separator();

    auto playlist_action = GUI::Action::create_checkable(ByteString { "播放列表" }, [this](auto&) {
        set_playlist_window_visible(!playlist_window_visible());
    });
    playlist_action->set_checked(playlist_window_visible());
    menu.add_action(playlist_action);

    auto lyrics_action = GUI::Action::create_checkable(ByteString { "歌词秀" }, [this](auto&) {
        set_lyrics_window_visible(!lyrics_window_visible());
    });
    lyrics_action->set_checked(lyrics_window_visible());
    menu.add_action(lyrics_action);

    auto equalizer_action = GUI::Action::create_checkable(ByteString { "均衡器" }, [this](auto&) {
        set_equalizer_window_visible(!equalizer_window_visible());
    });
    equalizer_action->set_checked(equalizer_window_visible());
    menu.add_action(equalizer_action);
    menu.add_separator();

    // The one entry that was unreachable without a menu bar: skin switching.
    auto skin_menu = menu.add_submenu("皮肤"_string);
    auto add_skin = [&](ByteString name, int index) {
        auto action = GUI::Action::create_checkable(move(name), [this, index](auto&) { apply_skin(index); });
        action->set_checked(SkinTheme::the().current_index() == index);
        skin_menu->add_action(action);
    };
    add_skin(ByteString { "内置经典" }, -1);
    for (int i = 0; i < SkinTheme::the().count(); ++i)
        add_skin(SkinTheme::the().name_at(i), i);
    skin_menu->add_separator();
    skin_menu->add_action(GUI::Action::create(ByteString { "从文件安装皮肤…" }, [this](auto&) { install_skin_package(); }));
    menu.add_separator();

    auto top_action = GUI::Action::create_checkable(ByteString { "窗口总在最前" }, [this](auto&) {
        bool next = !m_always_on_top;
        set_all_windows_on_top(next);
        Config::write_bool(config_domain, "Preferences"sv, "AlwaysOnTop"sv, next);
    });
    top_action->set_checked(m_always_on_top);
    menu.add_action(top_action);
    menu.add_separator();

    menu.add_action(GUI::CommonActions::make_about_action("千千静听"_string,
        GUI::Icon::default_icon("app-ttplayer"sv), this));
    menu.add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the()->quit();
    }));
}

void MainWindow::apply_skin_geometry()
{
    // Skins own the geometry: window size equals the backdrop bitmap size and
    // each sticky panel sits at its skin-defined offset from the main window.
    // Without this the bitmap is just stretched over a config-restored window.
    auto main_rect = SkinTheme::the().window_geometry(SkinTheme::WindowKind::Main);
    if (!main_rect.has_value())
        return;
    resize(main_rect->size());

    auto origin = position();
    auto place = [&](GUI::Window* panel, SkinTheme::WindowKind kind) {
        if (!panel)
            return;
        auto rect = SkinTheme::the().window_geometry(kind);
        if (!rect.has_value())
            return;
        // The skin owns the geometry, so its size is also the smallest a
        // panel may be. Without this the playlist window's own hard-coded
        // minimum (280x180) beat the skin's 268x165 and the four panels no
        // longer tiled into the 2x2 grid the skin draws.
        panel->set_minimum_size(rect->width(), rect->height());
        panel->resize(rect->size());
        panel->move_to(origin + rect->location());
    };
    place(m_playlist_window.ptr(), SkinTheme::WindowKind::Playlist);
    place(m_lyrics_window.ptr(), SkinTheme::WindowKind::Lyrics);
    place(m_equalizer_window.ptr(), SkinTheme::WindowKind::Equalizer);
}

void MainWindow::apply_window_shape()
{
    // Bitmap skins are shaped by their mask colour: tell WindowServer which
    // RGB is transparent so it both composites the holes and passes mouse
    // events through them. The classic look stays a plain opaque rectangle.
    auto color = SkinTheme::the().transparent_color();
    auto configure = [&](GUI::Window& window) {
        if (color.has_value()) {
            window.set_has_alpha_channel(true);
            window.set_alpha_hit_threshold(1.0f);
        } else {
            // Switching from a shaped skin back to a classic one must also
            // drop the alpha backing store. Leaving it on makes WindowServer
            // keep clearing the window to transparent and hit-testing against
            // alpha, so the "opaque" skin still shows see-through gaps.
            window.set_alpha_hit_threshold(0.0f);
            window.set_has_alpha_channel(false);
        }
        window.set_transparent_color(color);
    };
    configure(*this);
    if (m_playlist_window)
        configure(*m_playlist_window);
    if (m_lyrics_window)
        configure(*m_lyrics_window);
    if (m_equalizer_window)
        configure(*m_equalizer_window);
}

void MainWindow::set_all_windows_on_top(bool on_top)
{
    m_always_on_top = on_top;
    set_always_on_top(on_top);
    if (m_playlist_window)
        m_playlist_window->set_always_on_top(on_top);
    if (m_lyrics_window)
        m_lyrics_window->set_always_on_top(on_top);
    if (m_equalizer_window)
        m_equalizer_window->set_always_on_top(on_top);
}

void MainWindow::start_sticky_watch()
{
    // 千千静听 panels are separate top-level windows that travel with the main
    // window. There is no "moved" callback in this LibGUI, so poll the position
    // and shift every visible panel by the same delta.
    m_last_position = position();
    m_position_watch = Core::Timer::create_repeating(120, [this] {
        auto current = position();
        if (current == m_last_position)
            return;
        auto delta = current - m_last_position;
        m_last_position = current;
        auto move_panel = [&](GUI::Window* panel) {
            if (panel && panel->is_visible())
                panel->move_to(panel->position() + delta);
        };
        move_panel(m_playlist_window.ptr());
        move_panel(m_lyrics_window.ptr());
        move_panel(m_equalizer_window.ptr());
    });
    m_position_watch->start();
}

void MainWindow::repaint_all()
{
    if (auto* root = main_widget())
        repaint_tree(*root);
    if (m_playlist_window) {
        if (auto* root = m_playlist_window->main_widget())
            repaint_tree(*root);
    }
    if (m_lyrics_window) {
        if (auto* root = m_lyrics_window->main_widget())
            repaint_tree(*root);
    }
    if (m_equalizer_window) {
        if (auto* root = m_equalizer_window->main_widget())
            repaint_tree(*root);
    }
}

void MainWindow::wire_core()
{
    m_core->on_state_change = [this] {
        sync_from_core();
    };
    m_core->on_track_change = [this] {
        sync_from_core();
        if (m_lyrics_window)
            m_lyrics_window->refresh_lyrics();
    };
    m_core->on_position_change = [this] {
        sync_position();
    };
    m_core->on_playlist_change = [this] {
        if (m_playlist_window)
            m_playlist_window->refresh();
    };
    m_core->on_volume_change = [this] {
        if (m_volume)
            m_volume->set_value(static_cast<int>(m_core->volume() * 100));
    };
    m_core->on_eq_change = [this] {
        if (m_equalizer_window)
            m_equalizer_window->refresh();
    };
}

void MainWindow::update_native_canvas()
{
    // main_widget() is a plain pointer owned by the Window; do NOT keep an
    // extra RefPtr on it - that changes the destruction order and trips
    // RefCounted's m_ref_count > 0 assertion in EventReceiver::~EventReceiver.
    auto* panel = static_cast<SkinPanel*>(main_widget());
    if (!panel)
        return;
    auto* canvas = panel->native_canvas();
    if (!canvas || !canvas->has_layout())
        return;

    auto title = m_core->title();
    canvas->set_element_text("info"sv, title.is_empty() ? ByteString { "千千静听" } : title);
    canvas->set_element_text("status"sv, state_text_for(m_core->state()));
    canvas->set_element_text("stereo"sv, m_core->bitrate_kbps() > 0
            ? ByteString::formatted("立体声 {}kbps", m_core->bitrate_kbps())
            : ByteString { "立体声" });

    int position = m_core->position_ms();
    int duration = m_core->duration_ms();
    auto clock = [](int seconds) { return ByteString::formatted("{:02}:{:02}", seconds / 60, seconds % 60); };
    // Native LED strips hold a fixed number of glyph slots (Classic.skn: 5);
    // show elapsed time only, formatted with the sprite's ':' glyph.
    canvas->set_element_text("led"sv, clock(position / 1000));
    canvas->set_element_value("progress"sv, duration > 0 ? static_cast<float>(position) / duration : 0.0f);
    canvas->set_element_value("volume"sv, static_cast<float>(m_core->volume()));
}

void MainWindow::sync_from_core()
{
    m_info_box->set_info(m_core->title(), m_core->artist(), m_core->album());

    if (m_core->title().is_empty()) {
        m_marquee->set_text("千千静听 · 请添加音乐"sv);
    } else if (m_core->artist().is_empty()) {
        m_marquee->set_text(m_core->title());
    } else {
        m_marquee->set_text(ByteString::formatted("{} - {}", m_core->artist(), m_core->title()));
    }

    auto format = m_core->format_name();
    if (m_core->bitrate_kbps() > 0)
        m_status->set_format_text(ByteString::formatted("立体声 {}kbps", m_core->bitrate_kbps()));
    else if (!format.is_empty())
        m_status->set_format_text(ByteString::formatted("立体声 · {}", format));
    else
        m_status->set_format_text(ByteString { "立体声" });

    m_status->set_state_text(state_text_for(m_core->state()));
    m_transport->set_has_track(m_core->has_track());
    m_transport->set_playing(m_core->is_playing());

    m_seek->set_range(0, max(1, m_core->duration_ms()));
    sync_position();

    if (m_playlist_window)
        m_playlist_window->refresh();

    update_window_title();
    update_native_canvas();
}

void MainWindow::sync_position()
{
    update_native_canvas();
    if (!m_seek)
        return;
    int position = m_core->position_ms();
    int duration = m_core->duration_ms();
    if (!m_seek->is_dragging()) {
        m_seek->set_value(position);
        if (m_seek->max_value() != duration && duration > 0)
            m_seek->set_range(0, duration);
    }
    m_led->set_times(position / 1000, duration / 1000);
    if (m_lyrics_window)
        m_lyrics_window->set_position_ms(position);
}

void MainWindow::update_window_title()
{
    if (m_core->title().is_empty()) {
        set_title(ByteString { "千千静听" });
        return;
    }
    if (m_core->artist().is_empty()) {
        set_title(ByteString::formatted("{} - 千千静听", m_core->title()));
        return;
    }
    set_title(ByteString::formatted("{} - {} - 千千静听", m_core->artist(), m_core->title()));
}

// ---------------------------------------------------------------------------
// Secondary windows
// ---------------------------------------------------------------------------

void MainWindow::ensure_secondary_windows()
{
    auto screen = GUI::Desktop::the().rect();
    int right_x = min(x() + width() + 8, max(0, screen.width() - 350));
    int top_y = y();

    if (!m_playlist_window) {
        m_playlist_window = PlaylistWindow::create(m_core, { x(), min(y() + height() + 6, max(0, screen.height() - 310)) });
        m_playlist_window->on_context_menu_request = [this](auto& event) { show_context_menu(event); };
        m_playlist_window->on_close_request = [this] {
            set_playlist_window_visible(false);
            return GUI::Window::CloseRequestDecision::StayOpen;
        };
    }
    if (!m_lyrics_window) {
        m_lyrics_window = LyricsWindow::create(m_core, { right_x, top_y });
        m_lyrics_window->on_context_menu_request = [this](auto& event) { show_context_menu(event); };
        m_lyrics_window->on_close_request = [this] {
            set_lyrics_window_visible(false);
            return GUI::Window::CloseRequestDecision::StayOpen;
        };
    }
    if (!m_equalizer_window) {
        m_equalizer_window = EqualizerWindow::create(m_core, { right_x, min(top_y + 268, max(0, screen.height() - 224)) });
        m_equalizer_window->on_context_menu_request = [this](auto& event) { show_context_menu(event); };
        m_equalizer_window->on_close_request = [this] {
            set_equalizer_window_visible(false);
            return GUI::Window::CloseRequestDecision::StayOpen;
        };
    }

    set_all_windows_on_top(m_always_on_top);
    apply_window_shape();
    apply_skin_geometry();
}

bool MainWindow::playlist_window_visible() const
{
    return m_playlist_window && m_playlist_window->is_visible();
}

bool MainWindow::lyrics_window_visible() const
{
    return m_lyrics_window && m_lyrics_window->is_visible();
}

bool MainWindow::equalizer_window_visible() const
{
    return m_equalizer_window && m_equalizer_window->is_visible();
}

void MainWindow::set_playlist_window_visible(bool visible)
{
    ensure_secondary_windows();
    if (visible) {
        m_playlist_window->refresh();
        m_playlist_window->show();
    } else {
        m_playlist_window->hide();
    }
    if (m_playlist_toggle)
        m_playlist_toggle->set_checked(visible);
    if (m_playlist_window_action)
        m_playlist_window_action->set_checked(visible);
    Config::write_bool(config_domain, "Preferences"sv, "PlaylistWindowVisible"sv, visible);
}

void MainWindow::set_lyrics_window_visible(bool visible)
{
    ensure_secondary_windows();
    if (visible) {
        m_lyrics_window->refresh_lyrics();
        m_lyrics_window->show();
    } else {
        m_lyrics_window->hide();
    }
    if (m_lyrics_toggle)
        m_lyrics_toggle->set_checked(visible);
    if (m_lyrics_window_action)
        m_lyrics_window_action->set_checked(visible);
    Config::write_bool(config_domain, "Preferences"sv, "LyricsWindowVisible"sv, visible);
}

void MainWindow::set_equalizer_window_visible(bool visible)
{
    ensure_secondary_windows();
    if (visible) {
        m_equalizer_window->refresh();
        m_equalizer_window->show();
    } else {
        m_equalizer_window->hide();
    }
    if (m_eq_toggle)
        m_eq_toggle->set_checked(visible);
    if (m_equalizer_window_action)
        m_equalizer_window_action->set_checked(visible);
    Config::write_bool(config_domain, "Preferences"sv, "EqualizerWindowVisible"sv, visible);
}

void MainWindow::toggle_playlist_window()
{
    set_playlist_window_visible(!playlist_window_visible());
}

void MainWindow::toggle_lyrics_window()
{
    set_lyrics_window_visible(!lyrics_window_visible());
}

void MainWindow::toggle_equalizer_window()
{
    set_equalizer_window_visible(!equalizer_window_visible());
}

void MainWindow::sync_secondary_windows()
{
    if (m_playlist_toggle)
        m_playlist_toggle->set_checked(playlist_window_visible());
    if (m_lyrics_toggle)
        m_lyrics_toggle->set_checked(lyrics_window_visible());
    if (m_eq_toggle)
        m_eq_toggle->set_checked(equalizer_window_visible());
}
