/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>

// Playlist model for the TTPlayer rewrite.
//
// Modelled after the classic 千千静听 playlist: an ordered list of audio
// files plus a play mode (顺序播放 / 列表循环 / 单曲循环 / 随机播放). It knows
// nothing about the UI; the widget observes `on_change` and re-reads the
// entries.
class PlaylistStore {
public:
    struct Entry {
        ByteString path;
        Optional<ByteString> display_title;
        Optional<u32> duration_seconds;
        u64 file_size { 0 };
    };

    enum class PlayMode {
        Sequential,
        RepeatAll,
        RepeatOne,
        Shuffle,
    };

    static constexpr StringView mode_name(PlayMode mode)
    {
        switch (mode) {
        case PlayMode::Sequential:
            return "顺序播放"sv;
        case PlayMode::RepeatAll:
            return "列表循环"sv;
        case PlayMode::RepeatOne:
            return "单曲循环"sv;
        case PlayMode::Shuffle:
            return "随机播放"sv;
        }
        return ""sv;
    }

    static constexpr PlayMode mode_from_string(StringView value, PlayMode fallback = PlayMode::RepeatAll)
    {
        if (value == "sequential"sv)
            return PlayMode::Sequential;
        if (value == "repeat-all"sv)
            return PlayMode::RepeatAll;
        if (value == "repeat-one"sv)
            return PlayMode::RepeatOne;
        if (value == "shuffle"sv)
            return PlayMode::Shuffle;
        return fallback;
    }

    static constexpr StringView mode_to_string(PlayMode mode)
    {
        switch (mode) {
        case PlayMode::Sequential:
            return "sequential"sv;
        case PlayMode::RepeatAll:
            return "repeat-all"sv;
        case PlayMode::RepeatOne:
            return "repeat-one"sv;
        case PlayMode::Shuffle:
            return "shuffle"sv;
        }
        return "repeat-all"sv;
    }

    static PlayMode next_mode(PlayMode mode)
    {
        switch (mode) {
        case PlayMode::Sequential:
            return PlayMode::RepeatAll;
        case PlayMode::RepeatAll:
            return PlayMode::RepeatOne;
        case PlayMode::RepeatOne:
            return PlayMode::Shuffle;
        case PlayMode::Shuffle:
            return PlayMode::Sequential;
        }
        return PlayMode::RepeatAll;
    }

    void add_entry(Entry entry)
    {
        m_entries.append(move(entry));
        on_change();
    }

    void add_entries(Vector<Entry> entries);

    void remove_at(int index);
    void clear();

    // Fill in the duration once a track has been decoded by the player.
    // Fires on_change so the playlist view repaints the new time.
    void set_duration_seconds(int index, u32 seconds)
    {
        if (index < 0 || index >= size())
            return;
        auto& entry = m_entries[static_cast<size_t>(index)];
        if (entry.duration_seconds == seconds)
            return;
        entry.duration_seconds = seconds;
        on_change();
    }

    int size() const { return static_cast<int>(m_entries.size()); }
    bool is_empty() const { return m_entries.is_empty(); }
    Entry const* entry(int index) const
    {
        if (index < 0 || index >= size())
            return nullptr;
        return &m_entries[static_cast<size_t>(index)];
    }
    Vector<Entry> const& entries() const { return m_entries; }

    PlayMode mode() const { return m_mode; }
    void set_mode(PlayMode mode) { m_mode = mode; }

    // Index to play after `current` finishes. `manual` is true when the user
    // pressed the next button (RepeatOne is then ignored). Returns nullopt
    // when playback should stop (end of a non-repeating list).
    Optional<int> next_index(Optional<int> current, bool manual) const;
    // Index to play when the user presses the previous button.
    Optional<int> previous_index(int current) const;

    // M3U/M3U8 I/O. load_m3u8 replaces the current playlist.
    bool load_m3u8(StringView path);
    bool save_m3u8(StringView path) const;

    // Recursively scan `dir` for audio files (mp3/flac/wav/qoa). Depth is
    // limited so a symlink loop cannot hang the caller.
    static Vector<Entry> scan_folder(StringView dir, int max_depth = 8);

    Function<void()> on_change = [] {};

private:
    Vector<Entry> m_entries;
    PlayMode m_mode { PlayMode::RepeatAll };
    // Shuffle bag: indices to play in random order; refilled when empty.
    mutable Vector<int> m_shuffle_bag;
};
