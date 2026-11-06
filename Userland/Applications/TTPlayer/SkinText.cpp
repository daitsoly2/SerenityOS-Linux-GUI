/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinText.h"
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>

Gfx::Color SkinText::role_color() const
{
    auto const& colors = TTSkin::colors();
    switch (m_role) {
    case Role::Text:
        return colors.inset_text;
    case Role::Dim:
        return colors.inset_dim_text;
    case Role::Faint:
        return colors.faint_text;
    case Role::Accent:
        return colors.accent;
    }
    return colors.dim_text;
}

void SkinText::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    auto inner = frame_inner_rect();
    painter.fill_rect(inner, TTSkin::colors().inset_background);
    painter.draw_text(inner, m_text.view(), font(), Gfx::TextAlignment::CenterLeft, role_color());
}
