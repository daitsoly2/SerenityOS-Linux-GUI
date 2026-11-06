/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LyricsWindow.h"
#include "SkinButton.h"
#include "SkinPanel.h"
#include "SkinTheme.h"
#include "TTSkin.h"
#include <AK/LexicalPath.h>
#include <LibConfig/Client.h>
#include <LibGUI/BoxLayout.h>

LyricsWindow::LyricsWindow(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
    : m_core(move(core))
{
    set_title(ByteString { "歌词秀" });
    set_obey_widget_min_size(false);
    set_frameless(true);
    set_minimum_size(240, 160);
    restore_size_and_position("TTPlayer"sv, "LyricsWindow"sv, Gfx::IntSize { 340, 260 }, fallback_position);
    save_size_and_position_on_close("TTPlayer"sv, "LyricsWindow"sv);
    build_ui();
}

NonnullRefPtr<LyricsWindow> LyricsWindow::create(NonnullRefPtr<PlayerCore> core, Gfx::IntPoint fallback_position)
{
    return adopt_ref(*new LyricsWindow(move(core), fallback_position));
}

void LyricsWindow::build_ui()
{
    auto root_widget = set_main_widget<SkinPanel>();
    root_widget->set_kind(SkinTheme::WindowKind::Lyrics);
    root_widget->on_close_clicked = [this] { hide(); };
    root_widget->on_context_menu_request = [this](auto& event) {
        if (on_context_menu_request)
            on_context_menu_request(event);
    };
    auto& root = *root_widget;
    root.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14, 5, 5 }, 4);

    auto& header_row = root.add<GUI::Widget>();
    header_row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 4);
    header_row.set_fixed_height(20);

    auto add_button = [&](String text, int width, Function<void()> on_click) {
        auto& button = header_row.add<SkinButton>();
        button.set_text(text.to_byte_string());
        button.set_fixed_size(width, 18);
        button.on_click = move(on_click);
        return &button;
    };

    add_button("-0.5s"_string, 44, [this] { change_offset(-500); });
    add_button("+0.5s"_string, 44, [this] { change_offset(500); });
    add_button("重载"_string, 40, [this] { refresh_lyrics(); });

    m_header = &header_row.add<SkinText>(SkinText::Role::Accent, 18);
    m_header->set_text("歌词秀 · 未播放"sv);

    m_lyrics = &root.add<LyricsView>();
    m_lyrics->set_offset_ms(Config::read_i32("TTPlayer"sv, "Preferences"sv, "LyricOffsetMs"sv, 0));

    update_header();
}

int LyricsWindow::offset_ms() const
{
    return m_lyrics ? m_lyrics->offset_ms() : 0;
}

void LyricsWindow::change_offset(int delta_ms)
{
    if (!m_lyrics)
        return;
    auto offset = clamp(m_lyrics->offset_ms() + delta_ms, -20000, 20000);
    m_lyrics->set_offset_ms(offset);
    Config::write_i32("TTPlayer"sv, "Preferences"sv, "LyricOffsetMs"sv, offset);
    update_header();
}

void LyricsWindow::update_header()
{
    if (!m_header)
        return;
    auto title = m_core->title();
    if (title.is_empty()) {
        m_header->set_text("歌词秀 · 未播放"sv);
        return;
    }
    if (m_lyrics->has_lyrics())
        m_header->set_text(ByteString::formatted("{} · {}", title, ByteString::formatted("{}", m_lyrics->offset_ms() / 1000.0)));
    else
        m_header->set_text(ByteString::formatted("{} · 无歌词", title));
}

void LyricsWindow::refresh_lyrics()
{
    auto path = m_core->lyrics_path();
    if (path.is_empty())
        m_lyrics->load_lrc_file({});
    else
        m_lyrics->load_lrc_file(path);
    update_header();
    set_position_ms(m_core->position_ms());
}

void LyricsWindow::set_position_ms(int position_ms)
{
    if (m_lyrics)
        m_lyrics->set_position_ms(position_ms);
}
