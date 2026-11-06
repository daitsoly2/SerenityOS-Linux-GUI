/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "../SoundPlayer/PlaybackManager.h"
#include "PlaylistStore.h"
#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibAudio/ConnectionToServer.h>
#include <LibCore/EventReceiver.h>

namespace GUI {
class Window;
}

// Shared playback engine for the four 千千静听 windows (main / playlist /
// lyrics / equalizer).
//
// All UI state lives here so the windows can be independent GUI::Window
// instances that simply observe the callbacks below. Nothing in this class
// knows about widgets, which keeps the windows free to come and go.
class PlayerCore final : public Core::EventReceiver {
    C_OBJECT(PlayerCore)

public:
    enum class State {
        NoFileLoaded,
        Stopped,
        Playing,
        Paused,
    };

    // 10-band equalizer of the classic 千千静听 layout.
    static constexpr size_t eq_band_count = 10;
    static constexpr int eq_min_gain = -12;
    static constexpr int eq_max_gain = 12;

    explicit PlayerCore(NonnullRefPtr<Audio::ConnectionToServer>);
    virtual ~PlayerCore() override;

    PlaylistStore& store() { return m_store; }
    PlaylistStore const& store() const { return m_store; }

    State state() const { return m_state; }
    bool is_playing() const { return m_state == State::Playing; }
    bool has_track() const { return m_current_index.has_value(); }
    Optional<int> current_index() const { return m_current_index; }

    ByteString const& title() const { return m_title; }
    ByteString const& artist() const { return m_artist; }
    ByteString const& album() const { return m_album; }
    ByteString const& format_name() const { return m_format_name; }
    int sample_rate() const { return m_sample_rate; }
    int bitrate_kbps() const { return m_bitrate_kbps; }
    u64 file_size() const { return m_current_file_size; }

    int position_ms() const;
    int duration_ms() const { return m_total_ms; }

    void play_index(int index);
    void play_file_path(ByteString const& path);
    void toggle_play_pause();
    void stop_playback();
    void next_track();
    void previous_track();
    void seek_to_ms(int position_ms);

    void add_paths(Vector<ByteString> const& paths, bool play_first);
    // True when `path` is already in the playlist (case-sensitive).
    bool contains_path(ByteString const& path) const;
    void add_files(GUI::Window* parent);
    void add_folder(GUI::Window* parent);
    void open_playlist(GUI::Window* parent);
    void save_playlist(GUI::Window* parent);

    PlaylistStore::PlayMode play_mode() const { return m_store.mode(); }
    void set_play_mode(PlaylistStore::PlayMode);
    void cycle_play_mode();

    double volume() const { return m_volume; }
    void set_volume(double);
    // Writes the current volume to the config; call on drag end, not per pixel.
    void persist_volume();
    bool muted() const { return m_muted; }
    void set_muted(bool);

    // ---- Equalizer -------------------------------------------------------
    bool eq_enabled() const { return m_eq_enabled; }
    void set_eq_enabled(bool);
    int eq_preamp() const { return m_eq_preamp; }
    void set_eq_preamp(int db);
    int eq_band_gain(size_t band) const;
    void set_eq_band_gain(size_t band, int db);
    void reset_eq();
    static StringView eq_band_label(size_t band);

    Function<void()> on_state_change;
    Function<void()> on_track_change;
    Function<void()> on_position_change;
    Function<void()> on_playlist_change;
    Function<void()> on_volume_change;
    Function<void()> on_eq_change;

private:
    struct Biquad {
        float b0 { 1 }, b1 { 0 }, b2 { 0 }, a1 { 0 }, a2 { 0 };
        float z1[2] { 0, 0 };
        float z2[2] { 0, 0 };
        void reset();
        float process(float input, int channel);
    };

    void stop_internal();
    void handle_playback_finished();
    void resolve_metadata(PlaylistStore::Entry const&, ByteString const& path);
    void load_sibling_lyrics(ByteString const& path);
    void rebuild_eq_filters();
    void process_buffer(FixedArray<Audio::Sample>&);
    void save_session_playlist();
    void restore_session_playlist();
    void load_preferences();
    void save_preferences();

public:
    // The lyrics window observes this to know which .lrc to show.
    ByteString const& lyrics_path() const { return m_lyrics_path; }

private:
    NonnullRefPtr<Audio::ConnectionToServer> m_audio_client;
    PlaybackManager m_playback_manager;
    PlaylistStore m_store;

    State m_state { State::NoFileLoaded };
    Optional<int> m_current_index;
    ByteString m_title;
    ByteString m_artist;
    ByteString m_album;
    ByteString m_format_name;
    ByteString m_lyrics_path;
    int m_total_ms { 0 };
    int m_sample_rate { 44100 };
    int m_bitrate_kbps { 0 };
    u64 m_current_file_size { 0 };
    double m_volume { 0.8 };
    bool m_muted { false };

    bool m_eq_enabled { false };
    int m_eq_preamp { 0 };
    Array<int, eq_band_count> m_eq_gains {};
    Array<Biquad, eq_band_count> m_eq_filters {};
    float m_eq_preamp_linear { 1.0f };

    ByteString m_session_playlist_path;
};
