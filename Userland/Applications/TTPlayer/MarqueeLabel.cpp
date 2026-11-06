/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MarqueeLabel.h"
#include "TTSkin.h"
#include <LibGUI/Painter.h>
#include <math.h>

namespace {
constexpr int ScrollStep = 2;
constexpr int TextGap = 48; // pixels between the end and the re-entering start
}

void MarqueeLabel::timer_event(Core::TimerEvent&)
{
    if (m_text_width <= 0)
        return;
    int inner_width = frame_inner_rect().width();
    if (m_text_width <= inner_width)
        return;
    m_offset += ScrollStep;
    if (m_offset >= m_text_width + TextGap)
        m_offset = 0;
    update();
}

void MarqueeLabel::paint_event(GUI::PaintEvent& event)
{
    Frame::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto inner = frame_inner_rect();
    auto const& c = TTSkin::colors();
    painter.fill_rect(inner, c.inset_background);

    auto const& font = this->font();
    m_text_width = static_cast<int>(font.width(m_text));

    Gfx::Color text_color = c.inset_text;
    int inner_width = inner.width();

    if (m_text_width <= inner_width) {
        painter.draw_text(inner, m_text, font, Gfx::TextAlignment::CenterLeft, text_color);
        return;
    }

    int x = inner.x() - m_offset;
    while (x < inner.right()) {
        painter.draw_text(Gfx::IntRect(x, inner.y(), m_text_width, inner.height()),
            m_text, font, Gfx::TextAlignment::CenterLeft, text_color);
        x += m_text_width + TextGap;
    }
}
