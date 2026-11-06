/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TTSkin.h"
#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibGUI/Frame.h>

// Small readout label used on the skin (status strip, volume caption...).
// The colour is resolved from the active skin at paint time through a role, so
// switching skins repaints correctly without rebuilding the widget.
class SkinText final : public GUI::Frame {
    C_OBJECT(SkinText)

public:
    enum class Role {
        Text,
        Dim,
        Faint,
        Accent,
    };

    SkinText(Role role, int height)
        : m_role(role)
    {
        set_fixed_height(height);
    }

    virtual ~SkinText() override = default;

    void set_text(StringView text)
    {
        if (m_text == text)
            return;
        m_text = text;
        update();
    }

    StringView text() const { return m_text; }

    virtual void paint_event(GUI::PaintEvent& event) override;

private:
    Gfx::Color role_color() const;

    Role m_role { Role::Dim };
    ByteString m_text { "—" };
};
