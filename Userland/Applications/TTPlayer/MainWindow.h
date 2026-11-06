/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FlatSlider.h"
#include "LedDisplay.h"
#include "MarqueeLabel.h"
#include "PlayerCore.h"
#include "SkinButton.h"
#include "TransportBar.h"
#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Frame.h>
#include <LibCore/Timer.h>
#include <LibGUI/Window.h>

namespace GUI {
class Action;
class ActionGroup;
class ContextMenuEvent;
class Menu;
}

class PlaylistWindow;
class LyricsWindow;
class EqualizerWindow;

// Song info box of the classic main window: title, 艺术家 and 专辑.
class TrackInfoBox final : public GUI::Frame {
    C_OBJECT(TrackInfoBox)

public:
    virtual ~TrackInfoBox() override = default;

    void set_info(ByteString title, ByteString artist, ByteString album);

    virtual void paint_event(GUI::PaintEvent& event) override;

private:
    TrackInfoBox() = default;

    ByteString m_title;
    ByteString m_artist;
    ByteString m_album;
};

// "立体声 / 状态: 播放" readout next to the LED clock.
class StatusPanel final : public GUI::Widget {
    C_OBJECT(StatusPanel)

public:
    virtual ~StatusPanel() override = default;

    void set_state_text(ByteString text);
    void set_format_text(ByteString text);

    virtual void paint_event(GUI::PaintEvent& event) override;

private:
    StatusPanel() = default;

    ByteString m_state { "停止" };
    ByteString m_format { "立体声" };
};

// 千千静听 主窗口. Owns the playback core and the other three windows.
class MainWindow final : public GUI::Window {
    C_OBJECT(MainWindow)

public:
    virtual ~MainWindow() override;

    static NonnullRefPtr<MainWindow> create(NonnullRefPtr<PlayerCore>);

    void play_file_path(ByteString const& path);

    // Shared main menu for every skin panel. Frameless windows have no menu
    // bar, so this is the only way in (right-click, or the Menu key).
    void show_context_menu(GUI::ContextMenuEvent const& event);

    void toggle_playlist_window();
    void toggle_lyrics_window();
    void toggle_equalizer_window();
    void set_playlist_window_visible(bool);
    void set_lyrics_window_visible(bool);
    void set_equalizer_window_visible(bool);

    bool playlist_window_visible() const;
    bool lyrics_window_visible() const;
    bool equalizer_window_visible() const;

private:
    explicit MainWindow(NonnullRefPtr<PlayerCore>);

    void build_ui();
    void build_menus();
    void wire_core();
    void sync_from_core();
    void sync_position();
    void sync_title();
    void sync_secondary_windows();
    void ensure_secondary_windows();
    void update_window_title();
    void build_skin_menu(GUI::Menu& view_menu);
    void apply_skin(int index);
    void install_skin_package();
    void repaint_all();
    void start_sticky_watch();
    void build_context_menu();
    void set_all_windows_on_top(bool on_top);
    void apply_skin_geometry();

    NonnullRefPtr<PlayerCore> m_core;

    RefPtr<TrackInfoBox> m_info_box;
    RefPtr<LedDisplay> m_led;
    RefPtr<StatusPanel> m_status;
    RefPtr<MarqueeLabel> m_marquee;
    RefPtr<FlatSlider> m_seek;
    RefPtr<TransportBar> m_transport;
    RefPtr<FlatSlider> m_volume;
    RefPtr<SkinButton> m_playlist_toggle;
    RefPtr<SkinButton> m_lyrics_toggle;
    RefPtr<SkinButton> m_eq_toggle;

    RefPtr<PlaylistWindow> m_playlist_window;
    RefPtr<LyricsWindow> m_lyrics_window;
    RefPtr<EqualizerWindow> m_equalizer_window;

    GUI::ActionGroup m_mode_actions;
    GUI::ActionGroup m_skin_actions;
    RefPtr<GUI::Action> m_playlist_window_action;
    RefPtr<GUI::Action> m_lyrics_window_action;
    RefPtr<GUI::Action> m_equalizer_window_action;

    // Panels follow the main window like M4Player's sticky windows.
    RefPtr<Core::Timer> m_position_watch;
    Gfx::IntPoint m_last_position;

    // Rebuilt on each popup and kept alive until the menu item is activated.
    RefPtr<GUI::Menu> m_context_menu;
    bool m_always_on_top { false };

    bool m_suppress_seek_callback { false };
};
