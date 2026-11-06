/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TaskbarFrame.h"
#include <LibGUI/Painter.h>
#include <LibGfx/Palette.h>

void TaskbarFrame::paint_event(GUI::PaintEvent& event)
{
    // Windows 2000 tray: a sunken panel on the taskbar's button-face grey.
    // Fill first (Frame::paint_event only draws the border), so the tray reads
    // as a recessed area cut into the bar instead of a floating slab.
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), palette().button());
    Frame::paint_event(event);
}
