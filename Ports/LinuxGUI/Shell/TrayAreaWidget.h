/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include "TrayWidget.h"
#include <LibGUI/Button.h>
#include <LibGUI/Frame.h>
#include <LibGfx/Forward.h>

// The taskbar's notification area, now a real child widget.
//
// This used to be an empty sunken frame: the actual icons were top-level
// WindowType::Applet windows belonging to separate processes, positioned by
// WindowServer through set_applet_area_position(). Every change in the taskbar
// layout shifted the frame but the WM-side window kept its old coordinates, so
// the "native" tray icons wandered around. Everything in the tray is now a
// child widget of this frame; no second coordinate space is involved.
//
// The original applet processes still run for their side effects (clipboard
// history recording, for example) but their windows are no longer composited.
class TrayAreaWidget final : public GUI::Frame {
    C_OBJECT(TrayAreaWidget)

public:
    virtual ~TrayAreaWidget() override = default;

private:
    TrayAreaWidget();

    Vector<RefPtr<Gfx::Bitmap const>> m_icons;
    RefPtr<TrayWidget> m_tray;
    RefPtr<GUI::Button> m_previous;
    RefPtr<GUI::Button> m_next;
};
