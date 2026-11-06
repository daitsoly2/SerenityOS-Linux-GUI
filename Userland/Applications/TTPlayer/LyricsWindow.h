/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "LyricsView.h"
#include "PlayerCore.h"
#include "SkinText.h"
#include <AK/NonnullRefPtr.h>
#include <AK/Function.h>
#include <LibGUI/Event.h>
#include <LibGUI/Window.h>

// 千千静听 歌词秀 窗口: scrolling LRC with the current line highlighted.
class LyricsWindow final : public GUI::Window {
    C_OBJECT(LyricsWindow)

public:
    virtual ~LyricsWindow() override = default;

    static NonnullRefPtr<LyricsWindow> create(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    // Forwarded to MainWindow, which owns the player-wide context menu.
    Function<void(GUI::ContextMenuEvent&)> on_context_menu_request;

    // Called by the main window whenever a new track starts.
    void refresh_lyrics();
    // Called on every playback tick.
    void set_position_ms(int position_ms);
    int offset_ms() const;

private:
    LyricsWindow(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    void build_ui();
    void change_offset(int delta_ms);
    void update_header();

    NonnullRefPtr<PlayerCore> m_core;
    RefPtr<LyricsView> m_lyrics;
    RefPtr<SkinText> m_header;
};
