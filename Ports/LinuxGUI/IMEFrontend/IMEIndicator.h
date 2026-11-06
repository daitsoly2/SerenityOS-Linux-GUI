/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibGUI/Button.h>
#include <LibGUI/Menu.h>

namespace GUI {

struct IMEIndicatorMethod {
    ByteString unique_name;
    ByteString display_name;
};

// A taskbar tray indicator for the out-of-process input method (fcitx5 via the
// serenityime addon). Thin client only: it reads the current IME state from a
// status file the addon publishes and sends switch/toggle requests to the
// addon's control socket. No fcitx code is linked here.
//
// Visual: shows "中" when a CJK input method is active (you will type Chinese)
// and "EN" otherwise. Left click opens a menu of the available input methods.
class IMEIndicator final : public Button {
    C_OBJECT(IMEIndicator)
public:
    static ByteString status_directory();

    static ByteString status_path()
    {
        auto dir = status_directory();
        return ByteString::formatted("{}/serenity-ime-status", dir.view());
    }
    static ByteString methods_path()
    {
        auto dir = status_directory();
        return ByteString::formatted("{}/serenity-ime-methods", dir.view());
    }
    static ByteString control_socket_path()
    {
        auto dir = status_directory();
        return ByteString::formatted("{}/serenity-ime-ctl.sock", dir.view());
    }

    virtual ~IMEIndicator() override = default;

    // Re-read the status/methods files and repaint. Called by a timer and on demand.
    void refresh();

protected:
    virtual void paint_event(PaintEvent&) override;

private:
    IMEIndicator();

    static bool is_keyboard_layout(StringView unique_name);

    void open_switch_menu();
    void send_command(StringView command);

    bool m_active { false };
    ByteString m_unique_name;
    ByteString m_display_name;
    ByteString m_tag;

    RefPtr<Core::Timer> m_timer;

    // Keep the popup menu alive: Menu::popup() only records a raw pointer in its
    // static registry, so a local menu would be destroyed as soon as
    // open_switch_menu() returns and the popup would never appear.
    RefPtr<GUI::Menu> m_switch_menu;

    // Cached list of available input methods.
    Vector<IMEIndicatorMethod> m_methods;
};

}
