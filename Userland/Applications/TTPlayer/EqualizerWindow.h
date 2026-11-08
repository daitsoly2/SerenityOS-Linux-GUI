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
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>

// One vertical fader of the 10-band equalizer, painted in the classic
// 千千静听 style: thin groove, tick marks and a small steel grip.
class EqBandSlider final : public GUI::Widget {
    C_OBJECT(EqBandSlider)

public:
    virtual ~EqBandSlider() override = default;

    void set_label(ByteString label);
    void set_value(int value);
    int value() const { return m_value; }

    Function<void(int)> on_change;
    Function<void(int)> on_drag_end;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void mouseup_event(GUI::MouseEvent& event) override;

private:
    EqBandSlider() = default;

    int value_at(Gfx::IntPoint) const;
    Gfx::IntRect track_rect() const;

    ByteString m_label;
    int m_value { 0 };
    bool m_dragging { false };
};

// 千千静听 均衡器 窗口.
class EqualizerWindow final : public GUI::Window {
    C_OBJECT(EqualizerWindow)

public:
    virtual ~EqualizerWindow() override = default;

    static NonnullRefPtr<EqualizerWindow> create(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    // Forwarded to MainWindow, which owns the player-wide context menu.
    Function<void(GUI::ContextMenuEvent&)> on_context_menu_request;

    // Called when the core's equalizer settings changed from elsewhere.
    void refresh();

private:
    EqualizerWindow(NonnullRefPtr<PlayerCore>, Gfx::IntPoint fallback_position);

    void build_ui();
    void push_eq_to_canvas();

    NonnullRefPtr<PlayerCore> m_core;
    RefPtr<SkinButton> m_enable_button;
    RefPtr<EqBandSlider> m_preamp;
    Vector<NonnullRefPtr<EqBandSlider>> m_bands;
    class SkinPanel* m_panel { nullptr };
};
