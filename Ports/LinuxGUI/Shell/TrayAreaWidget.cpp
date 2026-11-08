/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TrayAreaWidget.h"
#include "StatusNotifierWatcher.h"
#include "TrayWidget.h"
#include <AK/Array.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibDesktop/DesktopIcons.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <Ports/LinuxGUI/IMEFrontend/IMEIndicator.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Palette.h>
#include <Ports/LinuxGUI/Common/hostify.h>

namespace {

// Tiny CPU / memory / network meters. The original ResourceGraph applet drew
// these as a separate top-level window; here they are just child widgets with
// a timer, so they cannot drift away from the taskbar.
class SystemMetersWidget final : public GUI::Widget {
    C_OBJECT(SystemMetersWidget)
public:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());

        constexpr Array<Gfx::Color, 3> colors { { { 0x4e, 0xc9, 0x4e }, { 0xc9, 0xb4, 0x4e }, { 0x4e, 0x9a, 0xc9 } } };
        for (int i = 0; i < 3; ++i) {
            auto bar = Gfx::IntRect { i * 5, 2, 3, height() - 4 };
            painter.fill_rect(bar, palette().button().darkened(0.75f));
            int filled = static_cast<int>((height() - 4) * clamp(m_values[i], 0.0f, 1.0f));
            if (filled > 0) {
                auto filled_rect = Gfx::IntRect { bar.x(), bar.bottom() - filled + 1, bar.width(), filled };
                painter.fill_rect(filled_rect, colors[i]);
            }
        }
    }

private:
    SystemMetersWidget()
    {
        set_fixed_size(17, 18);
        set_tooltip("CPU / 内存 / 网络"_string);
        sample();
        m_timer = Core::Timer::create_repeating(1000, [this] { sample(); });
        m_timer->start();
    }

    static u64 leading_number(StringView text)
    {
        u64 value = 0;
        bool found = false;
        for (char c : text.trim_whitespace()) {
            if (c >= '0' && c <= '9') {
                value = value * 10 + (c - '0');
                found = true;
            } else if (found) {
                break;
            }
        }
        return found ? value : 0;
    }

    void sample()
    {
        // CPU: delta of the first /proc/stat line (user+nice+system+idle...).
        auto cpu_file = Core::File::open("/proc/stat"sv, Core::File::OpenMode::Read);
        if (!cpu_file.is_error()) {
            if (auto content = cpu_file.value()->read_until_eof(); !content.is_error()) {
                auto line = StringView { content.value().bytes() }.trim_whitespace();
                auto rest = line.find(' ');
                if (rest.has_value()) {
                    u64 total = 0;
                    u64 idle = 0;
                    int field = 0;
                    for (auto part : line.substring_view(rest.value() + 1).split_view(' ')) {
                        auto number = part.to_number<u64>();
                        if (!number.has_value())
                            continue;
                        if (field == 3)
                            idle = *number;
                        total += *number;
                        ++field;
                    }
                    if (total > m_last_cpu_total) {
                        auto delta_idle = idle - m_last_cpu_idle;
                        auto delta_total = total - m_last_cpu_total;
                        m_values[0] = delta_total > 0 ? 1.0f - static_cast<float>(delta_idle) / delta_total : 0.0f;
                    }
                    m_last_cpu_total = total;
                    m_last_cpu_idle = idle;
                }
            }
        }

        // Memory: 1 - available/total.
        auto mem_file = Core::File::open("/proc/meminfo"sv, Core::File::OpenMode::Read);
        if (!mem_file.is_error()) {
            if (auto content = mem_file.value()->read_until_eof(); !content.is_error()) {
                u64 total = 0;
                u64 available = 0;
                for (auto line : StringView { content.value().bytes() }.split_view('\n')) {
                    auto colon = line.find(':');
                    if (!colon.has_value())
                        continue;
                    auto number = leading_number(line.substring_view(colon.value() + 1));
                    if (line.starts_with("MemTotal:"sv))
                        total = number;
                    else if (line.starts_with("MemAvailable:"sv))
                        available = number;
                }
                if (total > 0)
                    m_values[1] = 1.0f - static_cast<float>(available) / total;
            }
        }

        // Network: byte delta over one second, log-ish scale.
        auto net_file = Core::File::open("/proc/net/dev"sv, Core::File::OpenMode::Read);
        if (!net_file.is_error()) {
            if (auto content = net_file.value()->read_until_eof(); !content.is_error()) {
                u64 bytes = 0;
                bool first = true;
                for (auto line : StringView { content.value().bytes() }.split_view('\n')) {
                    if (first) {
                        first = false;
                        continue;
                    }
                    auto colon = line.find(':');
                    if (!colon.has_value() || line.find("lo:"sv).has_value())
                        continue;
                    for (auto field : line.substring_view(colon.value() + 1).split_view(' ')) {
                        auto number = field.to_number<u64>();
                        if (number.has_value()) {
                            bytes += *number;
                            break; // receive bytes only
                        }
                    }
                }
                if (m_last_net_bytes > 0)
                    m_values[2] = clamp(static_cast<float>(bytes - m_last_net_bytes) / (512.0f * 1024.0f), 0.0f, 1.0f);
                m_last_net_bytes = bytes;
            }
        }

        update();
    }

    RefPtr<Core::Timer> m_timer;
    Array<float, 3> m_values { 0.0f, 0.0f, 0.0f };
    u64 m_last_cpu_total { 0 };
    u64 m_last_cpu_idle { 0 };
    u64 m_last_net_bytes { 0 };
};

void launch(StringView executable)
{
    auto host_executable = PortHostify::resolve_executable(executable);
    Vector<char const*> no_arguments;
    if (auto result = Core::Process::spawn(host_executable.view(),
            ReadonlySpan<char const*> { no_arguments.data(), no_arguments.size() });
        result.is_error())
        warnln("tray: cannot launch {}: {}", host_executable, result.error());
}

}

TrayAreaWidget::TrayAreaWidget()
{
    set_frame_style(Gfx::FrameStyle::SunkenBox);
    set_fill_with_background_color(false);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 2, 1, 2, 1 }, 1);
    set_fixed_height(22);
    // Let our own layout decide the width. "Fit" resolves to
    // layout()->preferred_size(), so registering another StatusNotifierItem
    // widens the tray automatically; the taskbar's window-button area keeps
    // Grow and absorbs all the remaining space instead of sharing it with us.
    set_preferred_width(GUI::SpecialDimension::Fit);

    // Budget for the Linux StatusNotifierItem strip. The tray may never eat
    // the whole taskbar: window buttons need the rest. Derived from the screen
    // width so it scales with the display rather than being a fixed icon count.
    int budget = clamp(GUI::Desktop::the().rect().width() / 10, 2 * 18 + 2, 8 * 18 + 2);

    auto& previous = add<GUI::Button>();
    previous.set_text("<"_string);
    previous.set_button_style(Gfx::ButtonStyle::Normal);
    previous.set_fill_with_background_color(false);
    previous.set_fixed_size(12, 18);
    previous.set_tooltip("更多托盘图标"_string);
    previous.set_visible(false);

    auto& tray = add<TrayWidget>();
    tray.set_max_view_width(budget);

    auto& next = add<GUI::Button>();
    next.set_text(">"_string);
    next.set_button_style(Gfx::ButtonStyle::Normal);
    next.set_fill_with_background_color(false);
    next.set_fixed_size(12, 18);
    next.set_tooltip("更多托盘图标"_string);
    next.set_visible(false);

    m_previous = &previous;
    m_tray = &tray;
    m_next = &next;

    previous.on_click = [this](unsigned) { m_tray->scroll_by(-18); };
    next.on_click = [this](unsigned) { m_tray->scroll_by(18); };

    tray.on_items_changed = [this] {
        // Reveal the arrows only when there are icons outside the view, then
        // recompute the parent layout; our Fit width follows the strip.
        bool overflow = m_tray->can_scroll();
        m_previous->set_visible(overflow);
        m_next->set_visible(overflow);
        if (window() && window()->main_widget()) {
            window()->main_widget()->invalidate_layout();
            window()->main_widget()->do_layout();
        }
    };
    StatusNotifierWatcher::the().on_items_changed = [&tray] { tray.refresh(); };
    StatusNotifierWatcher::the().initialize();
    tray.refresh();

    // System meters (CPU / memory / network) as child widgets.
    auto& meters = add<SystemMetersWidget>();
    meters.set_visible(true);

    auto add_icon_button = [this](StringView icon_name, StringView tooltip, StringView executable, StringView fallback_text) {
        auto& button = add<GUI::Button>();
        button.set_button_style(Gfx::ButtonStyle::Normal);
        button.set_fill_with_background_color(false);
        button.set_fixed_size(20, 18);
        button.set_tooltip(MUST(String::from_byte_string(ByteString { tooltip })));
        // Desktop::icon() returns a RefPtr<Bitmap>; keep it alive for as long
        // as the button uses it (a local would leave a dangling pointer).
        // Desktop::icon() only knows icons compiled into its own table; load
        // the theme resource by name so a newly added icon just works.
        auto bitmap = Gfx::Bitmap::load_from_uri(
            ByteString::formatted("resource://icons/16x16/{}.png", icon_name));
        if (!bitmap.is_error()) {
            m_icons.append(bitmap.release_value());
            button.set_icon(m_icons.last().ptr());
        } else {
            button.set_text(MUST(String::from_byte_string(ByteString { fallback_text })));
        }
        button.on_click = [executable = ByteString { executable }](unsigned) { launch(executable); };
    };

    add_icon_button("network"sv, "网络"sv, "serenity-network"sv, "网"sv);
    add_icon_button("edit-copy"sv, "剪贴板历史"sv, "serenity-clipboard-history"sv, "剪"sv);

    // Volume: opens the standalone volume window anchored above the tray.
    auto& volume = add<GUI::Button>();
    volume.set_tooltip("Volume"_string);
    volume.set_button_style(Gfx::ButtonStyle::Normal);
    volume.set_fill_with_background_color(false);
    volume.set_fixed_size(20, 18);
    m_icons.append(Desktop::icon("audio-volume-high"sv));
    if (m_icons.last())
        volume.set_icon(m_icons.last().ptr());
    else
        volume.set_text("音"_string);
    volume.on_click = [&volume](unsigned) {
        constexpr int popup_width = 96;
        constexpr int popup_height = 240;
        auto button_rect = volume.screen_relative_rect();
        auto x = max(0, button_rect.center().x() - popup_width / 2);
        auto y = max(0, button_rect.top() - popup_height - 4);
        Array<ByteString, 2> arguments {
            ByteString { "--window" },
            ByteString::formatted("--position={},{}", x, y),
        };
        auto executable = PortHostify::resolve_executable("/bin/VolumeApplet"sv);
        if (auto result = Core::Process::spawn(executable.view(),
                ReadonlySpan<ByteString> { arguments.data(), arguments.size() });
            result.is_error())
            warnln("tray: cannot open volume: {}", result.error());
    };

    auto& ime = add<GUI::IMEIndicator>();
    ime.set_fill_with_background_color(false);
    ime.set_foreground_role(Gfx::ColorRole::TrayText);

}
