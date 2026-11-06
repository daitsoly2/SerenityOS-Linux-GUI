/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibGUI/Frame.h>

// 千千静听-style LED time display. Shows elapsed, remaining or total time
// with a blinking colon on a near-black panel. Click cycles the mode.
class LedDisplay final : public GUI::Frame {
    C_OBJECT(LedDisplay)

public:
    virtual ~LedDisplay() override = default;

    enum class Mode {
        Elapsed,
        Remaining,
        Total,
    };

    void set_times(int elapsed_seconds, int total_seconds);
    Mode mode() const { return m_mode; }
    void set_mode(Mode mode);

    Function<void()> on_mode_change = [] {};

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void timer_event(Core::TimerEvent&) override;

private:
    LedDisplay();

    void cycle_mode();

    Mode m_mode { Mode::Elapsed };
    int m_elapsed { 0 };
    int m_total { 0 };
    bool m_colon_visible { true };
};
