/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PlayerCore.h"
#include "SkinButton.h"
#include "SkinText.h"
#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibGUI/Widget.h>
#include <AK/Function.h>
#include <LibGUI/Event.h>
#include <LibGUI/Window.h>

namespace GUI {
class ScrollableContainerWidget;
}

// Custom-painted playlist rows: "# 序号 | 标题 | 时长", with the currently
// playing track on the classic light-blue band. A GUI::TableView would drag
// the whole system palette in and break the skin, so this paints itself.
class PlaylistListWidget final : public GUI::Widget {
    C_OBJECT(PlaylistListWidget)

public:
    virtual ~PlaylistListWidget() override = default;

    void set_core(PlayerCore& core) { m_core = &core; }
    void reload_rows();

    void set_current_index(Optional<int> index);
    Optional<int> selected_index() const { return m_selected >= 0 ? Optional<int>(m_selected) : Optional<int> {}; }
    void select_index(int index);

    Function<void(int index)> on_activate;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void doubleclick_event(GUI::MouseEvent& event) override;
    virtual void keydown_event(GUI::KeyEvent& event) override;

private:
    PlaylistListWidget() = default;

    static constexpr int row_height = 18;
    int row_at(Gfx::IntPoint) const;

    PlayerCore* m_core { nullptr };
    Optional<int> m_current;
    int m_selected { -1 };
};

// 千千静听 播放列表 窗口.
class PlaylistWindow final : public GUI::Window {
    C_OBJECT(PlaylistWindow)

public:
    virtual ~PlaylistWindow() override = default;

    static NonnullRefPtr<PlaylistWindow> create(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    // Forwarded to MainWindow, which owns the player-wide context menu.
    Function<void(GUI::ContextMenuEvent&)> on_context_menu_request;

    void refresh();

private:
    PlaylistWindow(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    void build_ui();
    void update_status();

    NonnullRefPtr<PlayerCore> m_core;
    RefPtr<PlaylistListWidget> m_list;
    RefPtr<GUI::ScrollableContainerWidget> m_scroller;
    RefPtr<SkinText> m_status_label;
    RefPtr<SkinButton> m_mode_button;
};
