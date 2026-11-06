/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGUI/Frame.h>

// Synchronized LRC lyrics view, in the spirit of the 千千静听 desktop lyric:
// the current line is highlighted and kept in the vertical center. Mouse
// wheel adjusts the time offset in 100 ms steps.
class LyricsView final : public GUI::Frame {
    C_OBJECT(LyricsView)

public:
    virtual ~LyricsView() override = default;

    // Load an .lrc file; passing nullopt clears the view.
    bool load_lrc_file(Optional<ByteString> path);

    // Playback position in milliseconds (incl. user offset).
    void set_position_ms(int position_ms);

    int offset_ms() const { return m_offset_ms; }
    void set_offset_ms(int offset_ms) { m_offset_ms = offset_ms; update(); }

    bool has_lyrics() const { return !m_entries.is_empty(); }
    ByteString const& lyrics_path() const { return m_lyrics_path; }

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void mousewheel_event(GUI::MouseEvent& event) override;

private:
    LyricsView();

    struct Entry {
        int time_ms { 0 };
        ByteString text;
    };

    int current_index(int position_ms) const;

    Vector<Entry> m_entries;
    ByteString m_lyrics_path;
    int m_offset_ms { 0 };
    int m_current_index { -1 };
    int m_position_ms { 0 };
};
