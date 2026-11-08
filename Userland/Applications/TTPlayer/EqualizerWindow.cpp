/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EqualizerWindow.h"
#include "SkinPanel.h"
#include "SkinTheme.h"
#include "TTSkin.h"
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>

namespace {
constexpr int label_height = 14;
}

void EqBandSlider::set_label(ByteString label)
{
    m_label = move(label);
    update();
}

void EqBandSlider::set_value(int value)
{
    value = clamp(value, PlayerCore::eq_min_gain, PlayerCore::eq_max_gain);
    if (m_value == value) {
        update();
        return;
    }
    m_value = value;
    update();
}

Gfx::IntRect EqBandSlider::track_rect() const
{
    return { width() / 2 - 2, 8, 4, max(10, height() - label_height - 12) };
}

int EqBandSlider::value_at(Gfx::IntPoint position) const
{
    auto track = track_rect();
    float ratio = 1.0f - (static_cast<float>(position.y() - track.y()) / static_cast<float>(track.height()));
    ratio = clamp(ratio, 0.0f, 1.0f);
    int range = PlayerCore::eq_max_gain - PlayerCore::eq_min_gain;
    return PlayerCore::eq_min_gain + static_cast<int>(ratio * static_cast<float>(range) + 0.5f);
}

void EqBandSlider::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto track = track_rect();
    int cx = width() / 2;

    // Recessed groove.
    painter.fill_rect(track, TTSkin::colors().inset_background);
    painter.draw_line(track.top_left(), track.bottom_left().translated(0, -1), TTSkin::colors().bevel_dark);
    painter.draw_line(track.top_right().translated(-1, 0), track.bottom_right().translated(-1, -1), TTSkin::colors().bevel_mid);

    // Tick marks every 3 dB, brighter at 0 dB.
    for (int db = PlayerCore::eq_min_gain; db <= PlayerCore::eq_max_gain; db += 3) {
        float ratio = static_cast<float>(db - PlayerCore::eq_min_gain) / static_cast<float>(PlayerCore::eq_max_gain - PlayerCore::eq_min_gain);
        int y = track.bottom() - 1 - static_cast<int>(ratio * static_cast<float>(track.height() - 1));
        bool const is_zero = db == 0;
        auto color = is_zero ? TTSkin::colors().dim_text : TTSkin::colors().faint_text;
        int half = is_zero ? 7 : 4;
        painter.draw_line({ cx - half, y }, { cx + half, y }, color);
    }

    // Grip.
    float ratio = static_cast<float>(m_value - PlayerCore::eq_min_gain) / static_cast<float>(PlayerCore::eq_max_gain - PlayerCore::eq_min_gain);
    int grip_y = track.bottom() - 1 - static_cast<int>(ratio * static_cast<float>(track.height() - 1));
    Gfx::IntRect grip { cx - 9, grip_y - 4, 18, 9 };
    TTSkin::paint_button(painter, grip, false, m_dragging);
    painter.draw_line({ grip.x() + 4, grip.center().y() }, { grip.right() - 5, grip.center().y() }, TTSkin::colors().text);

    // Band label under the fader.
    painter.draw_text(Gfx::IntRect { 0, height() - label_height, width(), label_height }, m_label.view(), font(),
        Gfx::TextAlignment::Center, m_dragging ? TTSkin::colors().accent : TTSkin::colors().dim_text);
}

void EqBandSlider::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::Primary) {
        GUI::Widget::mousedown_event(event);
        return;
    }
    m_dragging = true;
    int new_value = value_at(event.position());
    if (new_value != m_value) {
        m_value = new_value;
        if (on_change)
            on_change(m_value);
    }
    update();
    event.accept();
}

void EqBandSlider::mousemove_event(GUI::MouseEvent& event)
{
    if (!m_dragging) {
        GUI::Widget::mousemove_event(event);
        return;
    }
    int new_value = value_at(event.position());
    if (new_value != m_value) {
        m_value = new_value;
        if (on_change)
            on_change(m_value);
        update();
    }
    event.accept();
}

void EqBandSlider::mouseup_event(GUI::MouseEvent& event)
{
    if (m_dragging && event.button() == GUI::Primary) {
        m_dragging = false;
        update();
        if (on_drag_end)
            on_drag_end(m_value);
        event.accept();
        return;
    }
    GUI::Widget::mouseup_event(event);
}

// ---------------------------------------------------------------------------
// EqualizerWindow
// ---------------------------------------------------------------------------

EqualizerWindow::EqualizerWindow(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
    : m_core(move(core))
{
    set_title(ByteString { "均衡器" });
    set_skip_taskbar(true);
    set_obey_widget_min_size(false);
    set_resizable(false);
    set_frameless(true);
    build_ui();
    restore_size_and_position("TTPlayer"sv, "EqualizerWindow"sv, Gfx::IntSize { 340, 214 }, fallback_position);
    save_size_and_position_on_close("TTPlayer"sv, "EqualizerWindow"sv);
}

NonnullRefPtr<EqualizerWindow> EqualizerWindow::create(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
{
    return adopt_ref(*new EqualizerWindow(move(core), fallback_position));
}

void EqualizerWindow::build_ui()
{
    auto root_widget = set_main_widget<SkinPanel>();
    root_widget->set_kind(SkinTheme::WindowKind::Equalizer);
    root_widget->on_close_clicked = [this] { hide(); };
    root_widget->on_context_menu_request = [this](auto& event) {
        if (on_context_menu_request)
            on_context_menu_request(event);
    };
    auto& root = *root_widget;
    m_panel = root_widget;
    root.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14, 6, 6 }, 5);

    // Native skins carry the whole EQ chrome (including the baked-in
    // "Preamp 31 62 ... 16k" labels) and expect the player to move the
    // thumbs. The skin has no bitmap for that, so drive the canvas directly.
    root_widget->on_native_action = [this](StringView name) {
        if (name == "enabled"sv) {
            m_core->set_eq_enabled(!m_core->eq_enabled());
            refresh();
        } else if (name == "reset"sv) {
            m_core->reset_eq();
            refresh();
        } else if (name == "close"sv) {
            hide();
        }
    };
    root_widget->on_native_slider = [this](StringView name, float ratio) {
        int range = PlayerCore::eq_max_gain - PlayerCore::eq_min_gain;
        int gain = PlayerCore::eq_min_gain + static_cast<int>(ratio * static_cast<float>(range) + 0.5f);
        if (name == "preamp"sv) {
            m_core->set_eq_preamp(gain);
        } else if (name.starts_with("eqfactor"sv)) {
            auto band = name.substring_view(8).to_number<size_t>();
            if (band.has_value() && *band < PlayerCore::eq_band_count)
                m_core->set_eq_band_gain(*band, gain);
        }
        refresh();
    };
    root_widget->on_native_canvas_setup = [this](SkinCanvas&) { push_eq_to_canvas(); };

    auto& top_row = root.add<GUI::Widget>();
    top_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    top_row.set_fixed_height(22);

    m_enable_button = &top_row.add<SkinButton>();
    m_enable_button->set_text(ByteString { "开关" });
    m_enable_button->set_checkable(true);
    m_enable_button->set_fixed_size(52, 20);
    m_enable_button->on_click = [this] {
        m_core->set_eq_enabled(m_enable_button->is_checked());
    };

    auto& reset_button = top_row.add<SkinButton>();
    reset_button.set_text(ByteString { "复位" });
    reset_button.set_fixed_size(52, 20);
    reset_button.on_click = [this] {
        m_core->reset_eq();
        refresh();
    };

    top_row.add_spacer();
    auto& hint = top_row.add<SkinText>(SkinText::Role::Faint, 20);
    hint.set_text("10 段均衡 · 实时生效"sv);

    auto& sliders_row = root.add<GUI::Widget>();
    sliders_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 2 }, 4);

    m_preamp = &sliders_row.add<EqBandSlider>();
    m_preamp->set_label("前置"sv);
    m_preamp->set_fixed_width(30);
    m_preamp->set_value(m_core->eq_preamp());
    m_preamp->on_change = [this](int value) { m_core->set_eq_preamp(value); };

    for (size_t band = 0; band < PlayerCore::eq_band_count; band++) {
        auto& slider = sliders_row.add<EqBandSlider>();
        slider.set_label(PlayerCore::eq_band_label(band).to_byte_string());
        slider.set_fixed_width(26);
        slider.set_value(m_core->eq_band_gain(band));
        slider.on_change = [this, band](int value) { m_core->set_eq_band_gain(band, value); };
        m_bands.append(adopt_ref(slider));
    }

    m_enable_button->set_checked(m_core->eq_enabled());
    set_title(ByteString { "均衡器" });
}

void EqualizerWindow::refresh()
{
    if (!m_preamp)
        return;
    m_enable_button->set_checked(m_core->eq_enabled());
    m_preamp->set_value(m_core->eq_preamp());
    for (size_t band = 0; band < m_bands.size(); band++) {
        m_bands[band]->set_value(m_core->eq_band_gain(band));
    }
    push_eq_to_canvas();
}

void EqualizerWindow::push_eq_to_canvas()
{
    if (!m_panel)
        return;
    auto* canvas = m_panel->native_canvas();
    if (!canvas)
        return;
    auto to_ratio = [](int gain) {
        auto range = PlayerCore::eq_max_gain - PlayerCore::eq_min_gain;
        return static_cast<float>(gain - PlayerCore::eq_min_gain) / static_cast<float>(range);
    };
    canvas->set_element_value("preamp"sv, to_ratio(m_core->eq_preamp()));
    for (size_t band = 0; band < PlayerCore::eq_band_count; ++band)
        canvas->set_element_value(ByteString::formatted("eqfactor{}", band), to_ratio(m_core->eq_band_gain(band)));
}
