/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibGUI/Frame.h>

// Scrolling title label, like the 千千静听 info bar: when the text does not
// fit, it slides leftwards and wraps around with a gap.
class MarqueeLabel final : public GUI::Frame {
    C_OBJECT(MarqueeLabel)

public:
    virtual ~MarqueeLabel() override = default;

    void set_text(StringView text)
    {
        if (m_text == text)
            return;
        m_text = text;
        m_offset = 0;
        update();
    }

    StringView text() const { return m_text; }

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void timer_event(Core::TimerEvent&) override;

private:
    MarqueeLabel()
    {
        set_fixed_height(22);
        start_timer(40);
    }

    ByteString m_text;
    int m_offset { 0 };
    int m_text_width { 0 };
};
