/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlayerCore.h"
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/StringBuilder.h>
#include <LibAudio/Loader.h>
#include <LibConfig/Client.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Window.h>
#include <LibURL/URL.h>
#include <math.h>

namespace {

constexpr StringView config_domain = "TTPlayer"sv;

ByteString session_playlist_path()
{
    return ByteString::formatted("{}/serenity-ttplayer-session.m3u8", Core::StandardPaths::config_directory());
}

bool is_playlist_path(StringView path)
{
    return path.ends_with(".m3u"sv, AK::CaseSensitivity::CaseInsensitive)
        || path.ends_with(".m3u8"sv, AK::CaseSensitivity::CaseInsensitive);
}

// 千千静听 lists bands from 31 Hz to 16 kHz.
constexpr double eq_frequencies[PlayerCore::eq_band_count] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

constexpr double eq_q = 1.4;

}

// ---------------------------------------------------------------------------
// Biquad peaking filter
// ---------------------------------------------------------------------------

void PlayerCore::Biquad::reset()
{
    z1[0] = z1[1] = z2[0] = z2[1] = 0;
}

float PlayerCore::Biquad::process(float input, int channel)
{
    float output = b0 * input + z1[channel];
    z1[channel] = b1 * input - a1 * output + z2[channel];
    z2[channel] = b2 * input - a2 * output;
    return output;
}

// ---------------------------------------------------------------------------
// Construction / persistence
// ---------------------------------------------------------------------------

PlayerCore::PlayerCore(NonnullRefPtr<Audio::ConnectionToServer> connection)
    : m_audio_client(connection)
    , m_playback_manager(connection)
    , m_session_playlist_path(session_playlist_path())
{
    m_playback_manager.process_buffer = [this](FixedArray<Audio::Sample>& buffer) {
        process_buffer(buffer);
    };
    m_playback_manager.on_update = [this] {
        if (on_position_change)
            on_position_change();
    };
    m_playback_manager.on_finished_playing = [this] {
        handle_playback_finished();
    };

    m_store.on_change = [this] {
        save_session_playlist();
        if (on_playlist_change)
            on_playlist_change();
    };

    load_preferences();
    rebuild_eq_filters();
    restore_session_playlist();
}

PlayerCore::~PlayerCore()
{
    save_preferences();
    save_session_playlist();
}

void PlayerCore::load_preferences()
{
    auto volume_percent = Config::read_i32(config_domain, "Preferences"sv, "VolumePercent"sv, 80);
    m_volume = clamp(volume_percent, 0, 100) / 100.0;
    m_audio_client->set_self_volume(m_volume);

    m_muted = Config::read_bool(config_domain, "Preferences"sv, "Muted"sv, false);
    m_audio_client->set_self_muted(m_muted);

    m_store.set_mode(PlaylistStore::mode_from_string(Config::read_string(config_domain, "Preferences"sv, "PlayMode"sv, "repeat-all"sv)));

    m_eq_enabled = Config::read_bool(config_domain, "Preferences"sv, "EqEnabled"sv, false);
    m_eq_preamp = clamp(Config::read_i32(config_domain, "Preferences"sv, "EqPreamp"sv, 0), eq_min_gain, eq_max_gain);
    for (size_t band = 0; band < eq_band_count; band++) {
        auto key = ByteString::formatted("EqBand{}", band);
        m_eq_gains[band] = clamp(Config::read_i32(config_domain, "Preferences"sv, key.view(), 0), eq_min_gain, eq_max_gain);
    }
}

void PlayerCore::save_preferences()
{
    Config::write_i32(config_domain, "Preferences"sv, "VolumePercent"sv, static_cast<i32>(m_volume * 100));
    Config::write_bool(config_domain, "Preferences"sv, "Muted"sv, m_muted);
    Config::write_string(config_domain, "Preferences"sv, "PlayMode"sv, PlaylistStore::mode_to_string(m_store.mode()));
    Config::write_bool(config_domain, "Preferences"sv, "EqEnabled"sv, m_eq_enabled);
    Config::write_i32(config_domain, "Preferences"sv, "EqPreamp"sv, m_eq_preamp);
    for (size_t band = 0; band < eq_band_count; band++) {
        auto key = ByteString::formatted("EqBand{}", band);
        Config::write_i32(config_domain, "Preferences"sv, key.view(), m_eq_gains[band]);
    }
}

void PlayerCore::save_session_playlist()
{
    if (m_session_playlist_path.is_empty())
        return;
    (void)m_store.save_m3u8(m_session_playlist_path);
}

void PlayerCore::restore_session_playlist()
{
    if (FileSystem::exists(m_session_playlist_path))
        (void)m_store.load_m3u8(m_session_playlist_path);
}

// ---------------------------------------------------------------------------
// Equalizer
// ---------------------------------------------------------------------------

StringView PlayerCore::eq_band_label(size_t band)
{
    switch (band) {
    case 0: return "31"sv;
    case 1: return "62"sv;
    case 2: return "125"sv;
    case 3: return "250"sv;
    case 4: return "500"sv;
    case 5: return "1K"sv;
    case 6: return "2K"sv;
    case 7: return "4K"sv;
    case 8: return "8K"sv;
    case 9: return "16K"sv;
    }
    return ""sv;
}

int PlayerCore::eq_band_gain(size_t band) const
{
    if (band >= eq_band_count)
        return 0;
    return m_eq_gains[band];
}

void PlayerCore::set_eq_band_gain(size_t band, int db)
{
    if (band >= eq_band_count)
        return;
    db = clamp(db, eq_min_gain, eq_max_gain);
    if (m_eq_gains[band] == db)
        return;
    m_eq_gains[band] = db;
    rebuild_eq_filters();
    if (on_eq_change)
        on_eq_change();
}

void PlayerCore::set_eq_preamp(int db)
{
    db = clamp(db, eq_min_gain, eq_max_gain);
    if (m_eq_preamp == db)
        return;
    m_eq_preamp = db;
    rebuild_eq_filters();
    if (on_eq_change)
        on_eq_change();
}

void PlayerCore::set_eq_enabled(bool enabled)
{
    if (m_eq_enabled == enabled)
        return;
    m_eq_enabled = enabled;
    for (auto& filter : m_eq_filters)
        filter.reset();
    if (on_eq_change)
        on_eq_change();
}

void PlayerCore::reset_eq()
{
    m_eq_preamp = 0;
    m_eq_gains.fill(0);
    rebuild_eq_filters();
    if (on_eq_change)
        on_eq_change();
}

void PlayerCore::rebuild_eq_filters()
{
    // Coefficient computation needs the sample rate; recompute when it changes
    // (see play_index) as well as when a band moves.
    double const sample_rate = m_sample_rate > 0 ? m_sample_rate : 44100;
    for (size_t band = 0; band < eq_band_count; band++) {
        auto& filter = m_eq_filters[band];
        filter.reset();
        double const gain = m_eq_gains[band];
        if (gain == 0.0) {
            filter = {};
            continue;
        }
        double const frequency = min(eq_frequencies[band], sample_rate * 0.45);
        double const a = pow(10.0, gain / 40.0);
        double const w0 = 2.0 * M_PI * frequency / sample_rate;
        double const alpha = sin(w0) / (2.0 * eq_q);
        double const cos_w0 = cos(w0);
        double const a0 = 1.0 + alpha / a;
        filter.b0 = static_cast<float>((1.0 + alpha * a) / a0);
        filter.b1 = static_cast<float>((-2.0 * cos_w0) / a0);
        filter.b2 = static_cast<float>((1.0 - alpha * a) / a0);
        filter.a1 = static_cast<float>((-2.0 * cos_w0) / a0);
        filter.a2 = static_cast<float>((1.0 - alpha / a) / a0);
    }
    m_eq_preamp_linear = static_cast<float>(pow(10.0, m_eq_preamp / 20.0));
}

void PlayerCore::process_buffer(FixedArray<Audio::Sample>& buffer)
{
    if (on_audio_buffer)
        on_audio_buffer(buffer);
    if (!m_eq_enabled || buffer.is_empty())
        return;
    for (size_t band = 0; band < eq_band_count; band++) {
        if (m_eq_gains[band] == 0)
            continue;
        auto& filter = m_eq_filters[band];
        for (auto& sample : buffer) {
            sample.left = filter.process(sample.left, 0);
            sample.right = filter.process(sample.right, 1);
        }
    }
    if (m_eq_preamp_linear != 1.0f) {
        for (auto& sample : buffer) {
            sample.left = clamp(sample.left * m_eq_preamp_linear, -1.0f, 1.0f);
            sample.right = clamp(sample.right * m_eq_preamp_linear, -1.0f, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

int PlayerCore::position_ms() const
{
    auto loader = m_playback_manager.loader();
    if (!loader)
        return 0;
    auto rate = loader->sample_rate();
    if (rate == 0)
        return 0;
    return static_cast<int>(static_cast<long long>(loader->loaded_samples()) * 1000 / rate);
}

void PlayerCore::resolve_metadata(PlaylistStore::Entry const& entry, ByteString const& path)
{
    LexicalPath lexical_path(path);
    auto const& metadata = m_playback_manager.loader()->metadata();

    ByteString title = metadata.title.has_value() ? metadata.title->to_byte_string() : entry.display_title.value_or(lexical_path.title());
    ByteString artist = metadata.first_artist().has_value() ? metadata.first_artist()->to_byte_string() : ByteString {};
    ByteString album = metadata.album.has_value() ? metadata.album->to_byte_string() : ByteString {};

    // Fall back to the common "艺术家 - 曲名" file name convention.
    if (artist.is_empty()) {
        auto stem = entry.display_title.value_or(lexical_path.title());
        auto separator = stem.find(" - "sv);
        if (separator.has_value() && *separator > 0) {
            artist = stem.substring(0, *separator);
            if (!metadata.title.has_value())
                title = stem.substring(*separator + 3);
        }
    }

    m_title = title.is_empty() ? ByteString { lexical_path.title() } : title;
    m_artist = artist;
    m_album = album;

    ByteString format { m_playback_manager.loader()->format_name() };
    m_format_name = format.is_empty() ? ByteString { "AUDIO" } : format;

    if (m_total_ms > 0 && m_current_file_size > 0)
        m_bitrate_kbps = static_cast<int>((static_cast<double>(m_current_file_size) * 8.0) / (m_total_ms / 1000.0) / 1000.0);
    else
        m_bitrate_kbps = 0;
}

void PlayerCore::load_sibling_lyrics(ByteString const& path)
{
    LexicalPath lexical_path(path);
    m_lyrics_path = {};
    for (auto const& extension : { ".lrc"sv, ".LRC"sv }) {
        auto candidate = ByteString::formatted("{}/{}{}", lexical_path.dirname(), lexical_path.title(), extension);
        if (FileSystem::exists(candidate)) {
            m_lyrics_path = move(candidate);
            break;
        }
    }
}

void PlayerCore::play_index(int index)
{
    auto* entry = m_store.entry(index);
    if (!entry)
        return;
    ByteString path = entry->path;

    auto maybe_loader = Audio::Loader::create(path);
    if (maybe_loader.is_error()) {
        GUI::MessageBox::show(nullptr,
            ByteString::formatted("无法播放「{}」：\n{}", LexicalPath::title(path), maybe_loader.error().description),
            "千千静听"sv,
            GUI::MessageBox::Type::Error);
        return;
    }
    auto loader = maybe_loader.value();

    m_sample_rate = static_cast<int>(loader->sample_rate());
    auto total_samples = loader->total_samples();
    m_total_ms = m_sample_rate > 0 ? static_cast<int>(static_cast<long long>(total_samples) * 1000 / m_sample_rate) : 0;
    m_current_file_size = entry->file_size;

    // Stops any current playback and resets the pipeline.
    m_playback_manager.set_loader(move(loader));
    m_current_index = index;
    m_state = State::Playing;
    m_playback_manager.play();

    resolve_metadata(*entry, path);
    load_sibling_lyrics(path);
    rebuild_eq_filters();

    // Now that the header has been parsed we know the exact length.
    if (m_total_ms > 0)
        m_store.set_duration_seconds(index, static_cast<u32>((m_total_ms + 500) / 1000));

    if (on_track_change)
        on_track_change();
    if (on_state_change)
        on_state_change();
    if (on_position_change)
        on_position_change();
}

void PlayerCore::play_file_path(ByteString const& path)
{
    if (is_playlist_path(path)) {
        if (m_store.load_m3u8(path)) {
            auto first = m_store.next_index({}, /*manual=*/true);
            if (first.has_value())
                play_index(*first);
        } else {
            GUI::MessageBox::show(nullptr,
                ByteString::formatted("无法载入播放列表「{}」", path),
                "千千静听"sv,
                GUI::MessageBox::Type::Error);
        }
        return;
    }

    // Opening a file that is already queued (e.g. restored session playlist +
    // command line) must not add a duplicate row: just jump to it.
    if (contains_path(path)) {
        for (size_t i = 0; i < m_store.entries().size(); i++) {
            if (m_store.entries()[i].path == path) {
                play_index(static_cast<int>(i));
                break;
            }
        }
        return;
    }

    PlaylistStore::Entry entry;
    entry.path = path;
    entry.display_title = LexicalPath::title(path);
    if (auto size = FileSystem::size_from_stat(path); !size.is_error())
        entry.file_size = static_cast<u64>(size.value());
    m_store.add_entries({ move(entry) });
    play_index(m_store.size() - 1);
}

void PlayerCore::handle_playback_finished()
{
    auto next = m_store.next_index(m_current_index, /*manual=*/false);
    if (next.has_value()) {
        play_index(*next);
        return;
    }
    stop_internal();
}

void PlayerCore::stop_internal()
{
    m_playback_manager.stop();
    m_state = has_track() ? State::Stopped : State::NoFileLoaded;
    if (on_state_change)
        on_state_change();
    if (on_position_change)
        on_position_change();
}

void PlayerCore::toggle_play_pause()
{
    switch (m_state) {
    case State::NoFileLoaded:
        if (m_store.size() > 0)
            play_index(m_current_index.value_or(0));
        break;
    case State::Playing:
        m_playback_manager.pause();
        m_state = State::Paused;
        if (on_state_change)
            on_state_change();
        break;
    case State::Paused:
        m_playback_manager.play();
        m_state = State::Playing;
        if (on_state_change)
            on_state_change();
        break;
    case State::Stopped:
        if (m_current_index.has_value())
            play_index(*m_current_index);
        else if (m_store.size() > 0)
            play_index(0);
        break;
    }
}

void PlayerCore::stop_playback()
{
    stop_internal();
}

void PlayerCore::next_track()
{
    auto next = m_store.next_index(m_current_index, /*manual=*/true);
    if (next.has_value())
        play_index(*next);
}

void PlayerCore::previous_track()
{
    // More than 3 seconds in: restart the current track instead.
    if (position_ms() > 3000) {
        seek_to_ms(0);
        return;
    }
    auto previous = m_store.previous_index(m_current_index.value_or(0));
    if (previous.has_value())
        play_index(*previous);
}

void PlayerCore::seek_to_ms(int position_ms)
{
    if (m_state == State::NoFileLoaded || m_sample_rate <= 0)
        return;
    auto sample_position = static_cast<int>(static_cast<long long>(max(0, position_ms)) * m_sample_rate / 1000);
    m_playback_manager.seek(sample_position);
    for (auto& filter : m_eq_filters)
        filter.reset();
    if (on_position_change)
        on_position_change();
}

void PlayerCore::set_play_mode(PlaylistStore::PlayMode mode)
{
    m_store.set_mode(mode);
    Config::write_string(config_domain, "Preferences"sv, "PlayMode"sv, PlaylistStore::mode_to_string(mode));
    if (on_state_change)
        on_state_change();
}

void PlayerCore::cycle_play_mode()
{
    set_play_mode(PlaylistStore::next_mode(m_store.mode()));
}

void PlayerCore::set_volume(double volume)
{
    // 100% is unity gain on the server side; anything above that only adds
    // hard clipping on loud masters, so the slider stops there.
    double new_volume = clamp(volume, 0.0, 1.0);
    if (m_volume == new_volume)
        return;
    m_volume = new_volume;
    m_audio_client->set_self_volume(m_volume);
    if (on_volume_change)
        on_volume_change();
}

void PlayerCore::persist_volume()
{
    Config::write_i32(config_domain, "Preferences"sv, "VolumePercent"sv, static_cast<i32>(m_volume * 100));
}

void PlayerCore::set_muted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    m_audio_client->set_self_muted(m_muted);
    Config::write_bool(config_domain, "Preferences"sv, "Muted"sv, m_muted);
    if (on_volume_change)
        on_volume_change();
}

// ---------------------------------------------------------------------------
// Playlist commands
// ---------------------------------------------------------------------------

bool PlayerCore::contains_path(ByteString const& path) const
{
    for (auto const& entry : m_store.entries()) {
        if (entry.path == path)
            return true;
    }
    return false;
}

void PlayerCore::add_paths(Vector<ByteString> const& paths, bool play_first)
{
    Vector<PlaylistStore::Entry> entries;
    for (auto const& path : paths) {
        if (is_playlist_path(path)) {
            (void)m_store.load_m3u8(path);
            continue;
        }
        if (contains_path(path))
            continue;
        PlaylistStore::Entry entry;
        entry.path = path;
        entry.display_title = LexicalPath::title(path);
        if (auto size = FileSystem::size_from_stat(path); !size.is_error())
            entry.file_size = static_cast<u64>(size.value());
        entries.append(move(entry));
    }

    if (entries.is_empty())
        return;
    int const first_new_index = m_store.size();
    m_store.add_entries(move(entries));
    if (play_first)
        play_index(first_new_index);
}

void PlayerCore::add_files(GUI::Window* parent)
{
    GUI::FileTypeFilter audio_filter { "音频文件", Vector<ByteString> { "mp3", "flac", "wav", "qoa", "m3u", "m3u8" } };
    auto maybe_path = GUI::FilePicker::get_open_filepath(parent, "添加音频文件",
        Core::StandardPaths::music_directory(), /*folder=*/false,
        GUI::Dialog::ScreenPosition::CenterWithinParent,
        Vector<GUI::FileTypeFilter> { move(audio_filter) });
    if (!maybe_path.has_value())
        return;
    add_paths({ move(*maybe_path) }, /*play_first=*/false);
}

void PlayerCore::add_folder(GUI::Window* parent)
{
    auto maybe_path = GUI::FilePicker::get_open_filepath(parent, "选择音乐目录",
        Core::StandardPaths::music_directory(), /*folder=*/true,
        GUI::Dialog::ScreenPosition::CenterWithinParent);
    if (!maybe_path.has_value())
        return;
    auto entries = PlaylistStore::scan_folder(*maybe_path);
    if (entries.is_empty()) {
        GUI::MessageBox::show(parent, "该目录下没有找到可播放的音频文件。"sv,
            "千千静听"sv, GUI::MessageBox::Type::Information);
        return;
    }
    m_store.add_entries(move(entries));
}

void PlayerCore::open_playlist(GUI::Window* parent)
{
    GUI::FileTypeFilter playlist_filter { "播放列表", Vector<ByteString> { "m3u", "m3u8" } };
    auto maybe_path = GUI::FilePicker::get_open_filepath(parent, "打开播放列表",
        Core::StandardPaths::music_directory(), /*folder=*/false,
        GUI::Dialog::ScreenPosition::CenterWithinParent,
        Vector<GUI::FileTypeFilter> { move(playlist_filter) });
    if (!maybe_path.has_value())
        return;
    if (!m_store.load_m3u8(*maybe_path)) {
        GUI::MessageBox::show(parent,
            ByteString::formatted("无法载入播放列表「{}」", *maybe_path),
            "千千静听"sv,
            GUI::MessageBox::Type::Error);
    }
}

void PlayerCore::save_playlist(GUI::Window* parent)
{
    auto maybe_path = GUI::FilePicker::get_save_filepath(parent, "保存播放列表", "m3u8",
        Core::StandardPaths::music_directory());
    if (!maybe_path.has_value())
        return;
    if (!m_store.save_m3u8(*maybe_path)) {
        GUI::MessageBox::show(parent,
            ByteString::formatted("无法写入「{}」", *maybe_path),
            "千千静听"sv,
            GUI::MessageBox::Type::Error);
    }
}
