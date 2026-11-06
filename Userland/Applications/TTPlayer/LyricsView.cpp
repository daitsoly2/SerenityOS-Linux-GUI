/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LyricsView.h"
#include <AK/QuickSort.h>
#include <AK/StringView.h>
#include <LibCore/File.h>
#include <LibTextCodec/Decoder.h>
#include <LibGUI/Event.h>
#include "TTSkin.h"
#include <LibGUI/Painter.h>
#include <stdio.h>
#include <string.h>

namespace {

// Parse "[mm:ss]", "[mm:ss.xx]" or "[mm:ss.xxx]". Returns {} on failure.
Optional<int> parse_lrc_time(StringView section)
{
    auto parts = section.split_view(':');
    if (parts.size() < 2)
        return {};
    auto minutes = parts[0].to_number<int>();
    if (!minutes.has_value())
        return {};
    // The seconds part may itself contain a fraction separator ("ss.xx").
    StringView seconds_view = parts[1];
    auto dot = seconds_view.find('.');
    if (!dot.has_value())
        dot = seconds_view.find(':');
    StringView whole = dot.has_value() ? seconds_view.substring_view(0, *dot) : seconds_view;
    auto seconds = whole.to_number<int>();
    if (!seconds.has_value() || *seconds >= 60)
        return {};
    int millis = 0;
    if (dot.has_value()) {
        auto frac = seconds_view.substring_view(*dot + 1);
        // Normalize to milliseconds: "xx" -> xx * 10, "xxx" -> xxx.
        auto frac_number = frac.to_number<int>();
        if (frac_number.has_value()) {
            if (frac.length() == 1)
                millis = *frac_number * 100;
            else if (frac.length() == 2)
                millis = *frac_number * 10;
            else
                millis = *frac_number;
        }
    }
    return *minutes * 60 * 1000 + *seconds * 1000 + millis;
}

constexpr int LineHeight = 24;

}

LyricsView::LyricsView()
{
    set_fill_with_background_color(true);
}

bool LyricsView::load_lrc_file(Optional<ByteString> path)
{
    m_entries.clear();
    m_lyrics_path = {};
    m_current_index = -1;

    if (!path.has_value()) {
        update();
        return false;
    }

    auto maybe_file = Core::File::open(*path, Core::File::OpenMode::Read);
    if (maybe_file.is_error()) {
        update();
        return false;
    }
    auto file = maybe_file.release_value();
    auto maybe_bytes = file->read_until_eof();
    if (maybe_bytes.is_error()) {
        update();
        return false;
    }
    auto bytes = maybe_bytes.release_value();

    // Chinese .lrc files are usually GBK/GB18030 rather than UTF-8. Try a
    // strict UTF-8 decode first, then fall back to GB18030 (a superset of
    // GBK), and only then to a lossy UTF-8 decode.
    StringView raw_view { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    String content;
    if (auto utf8 = String::from_utf8(raw_view); !utf8.is_error()) {
        content = utf8.release_value();
    } else {
        TextCodec::GB18030Decoder decoder;
        if (auto gb = decoder.to_utf8(raw_view); !gb.is_error())
            content = gb.release_value();
        else
            content = String::from_utf8_with_replacement_character(raw_view);
    }
    ByteString text = content.to_byte_string();

    for (auto const& raw_line : text.view().split_view('\n', AK::SplitBehavior::Nothing)) {
        StringView line = raw_line;
        line = line.trim_whitespace();
        if (line.is_empty())
            continue;

        Vector<int> times;
        size_t pos = 0;
        while (pos < line.length() && line[pos] == '[') {
            auto close = line.find(']', pos);
            if (!close.has_value())
                break;
            auto section = line.substring_view(pos + 1, *close - pos - 1);
            auto time = parse_lrc_time(section);
            if (time.has_value())
                times.append(*time);
            else
                break; // Not a time tag (e.g. [ti:...]): stop consuming.
            pos = *close + 1;
        }

        if (times.is_empty())
            continue;

        auto lyric_text = line.substring_view(pos).trim_whitespace();
        for (auto time : times)
            m_entries.append({ time, lyric_text.to_byte_string() });
    }

    AK::quick_sort(m_entries, [](Entry const& a, Entry const& b) { return a.time_ms < b.time_ms; });
    m_lyrics_path = *path;
    update();
    return !m_entries.is_empty();
}

int LyricsView::current_index(int position_ms) const
{
    int low = 0;
    int high = static_cast<int>(m_entries.size()) - 1;
    int result = -1;
    while (low <= high) {
        int mid = (low + high) / 2;
        if (m_entries[static_cast<size_t>(mid)].time_ms <= position_ms) {
            result = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return result;
}

void LyricsView::set_position_ms(int position_ms)
{
    m_position_ms = position_ms;
    int adjusted = position_ms + m_offset_ms;
    int index = m_entries.is_empty() ? -1 : current_index(adjusted);
    if (index != m_current_index) {
        m_current_index = index;
        update();
    }
}

void LyricsView::mousewheel_event(GUI::MouseEvent& event)
{
    int delta = event.wheel_delta_y();
    if (delta != 0) {
        m_offset_ms += (delta > 0 ? -1 : 1) * 100;
        m_current_index = -2; // force re-evaluation with the new offset
        set_position_ms(m_position_ms);
        event.accept();
        return;
    }
    Frame::mousewheel_event(event);
}

void LyricsView::paint_event(GUI::PaintEvent& event)
{
    Frame::paint_event(event);
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto inner = frame_inner_rect();
    auto const& c = TTSkin::colors();
    painter.fill_rect(inner, c.inset_background);

    if (m_entries.is_empty()) {
        painter.draw_text(inner, "♪ 暂无歌词"sv, font().bold_variant(),
            Gfx::TextAlignment::Center, c.faint_text);
        painter.draw_text(Gfx::IntRect(inner.x(), inner.y() + inner.height() / 2 + 14, inner.width(), 16),
            "把与音频同名的 .lrc 文件放在旁边即可显示"sv, font(),
            Gfx::TextAlignment::Center, c.faint_text);
        return;
    }

    int const center_y = inner.y() + inner.height() / 2;
    int const current = m_current_index;

    // Draw lines around the current position.
    for (int row = -4; row <= 4; row++) {
        int index = current + row;
        if (index < 0 || index >= static_cast<int>(m_entries.size()))
            continue;
        int y = center_y + row * LineHeight - LineHeight / 2;

        Gfx::Color color;
        Gfx::Font const* font_ptr = &font();
        if (row == 0) {
            // Classic 歌词秀: the active line is bright green and bold.
            color = c.accent;
            font_ptr = &font().bold_variant();
        } else {
            // Fade distant lines towards the background.
            static constexpr u8 alphas[] = { 0, 0xe0, 0xb0, 0x88, 0x60 };
            color = Gfx::Color(c.inset_text.red(), c.inset_text.green(), c.inset_text.blue(),
                alphas[min(abs(row), 4)]);
        }

        painter.draw_text(Gfx::IntRect(inner.x() + 8, y, inner.width() - 16, LineHeight),
            m_entries[static_cast<size_t>(index)].text, *font_ptr,
            Gfx::TextAlignment::Center, color);
    }

    // Lines above the first lyric are shown as a waiting state.
    if (current < 0) {
        painter.draw_text(Gfx::IntRect(inner.x() + 8, center_y - LineHeight / 2, inner.width() - 16, LineHeight),
            "♪ 前奏 · 准备播放"sv, Gfx::TextAlignment::Center, c.faint_text);
    }

    // Time-offset indicator (only when non-zero).
    if (m_offset_ms != 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%+.1fs", static_cast<double>(m_offset_ms) / 1000.0);
        painter.draw_text(Gfx::IntRect(inner.right() - 56, inner.y() + 2, 52, 12),
            StringView { buffer, strlen(buffer) }, font(), Gfx::TextAlignment::CenterRight, c.inset_dim_text);
    }
}
