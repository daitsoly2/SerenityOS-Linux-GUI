/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGUI/Widget.h>

// Classic 千千静听 transport strip: a row of small steel buttons
// [◀◀] [▶] [‖] [■] [▶▶] [⏏] painted by hand, no bitmaps.
class TransportBar final : public GUI::Widget {
    C_OBJECT(TransportBar)

public:
    enum class Button {
        Previous,
        Play,
        Pause,
        Stop,
        Next,
        Eject,
    };

    virtual ~TransportBar() override = default;

    void set_playing(bool playing);
    void set_has_track(bool has_track);

    Function<void()> on_previous;
    Function<void()> on_play;
    Function<void()> on_pause;
    Function<void()> on_stop;
    Function<void()> on_next;
    Function<void()> on_eject;

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mouseup_event(GUI::MouseEvent& event) override;
    virtual void leave_event(Core::Event& event) override;

private:
    static constexpr int button_width = 26;
    static constexpr int button_gap = 3;

    TransportBar()
    {
        set_fixed_height(26);
        set_fixed_width(6 * button_width + 5 * button_gap + 2);
    }

    Gfx::IntRect button_rect(Button) const;
    Optional<Button> button_at(Gfx::IntPoint) const;
    bool is_enabled(Button) const;
    void draw_glyph(GUI::Painter&, Button, Gfx::IntRect, Gfx::Color) const;

    bool m_playing { false };
    bool m_has_track { false };
    Optional<Button> m_hovered;
    Optional<Button> m_pressed;
};
