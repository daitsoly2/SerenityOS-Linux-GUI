/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaylistStore.h"
#include "../SoundPlayer/M3UParser.h"
#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/Random.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>
#include <LibCore/File.h>
#include <LibCore/DirIterator.h>
#include <LibFileSystem/FileSystem.h>

namespace {

bool is_audio_file(ByteString const& path)
{
    auto extension = LexicalPath::extension(path);
    static constexpr auto audio_extensions = Array { "mp3"sv, "flac"sv, "wav"sv, "qoa"sv };
    for (auto const& audio_extension : audio_extensions) {
        if (extension.equals_ignoring_ascii_case(audio_extension))
            return true;
    }
    return false;
}

u64 file_size_or_zero(StringView path)
{
    auto size = FileSystem::size_from_stat(path);
    return size.is_error() ? 0 : static_cast<u64>(size.value());
}

}

void PlaylistStore::add_entries(Vector<Entry> entries)
{
    if (entries.is_empty())
        return;
    for (auto& entry : entries)
        m_entries.append(move(entry));
    on_change();
}

void PlaylistStore::remove_at(int index)
{
    if (index < 0 || index >= size())
        return;
    m_entries.remove(static_cast<size_t>(index));
    on_change();
}

void PlaylistStore::clear()
{
    if (m_entries.is_empty())
        return;
    m_entries.clear();
    on_change();
}

Optional<int> PlaylistStore::next_index(Optional<int> current, bool manual) const
{
    int count = size();
    if (count == 0)
        return {};

    if (!manual && m_mode == PlayMode::RepeatOne && current.has_value())
        return current;

    if (m_mode == PlayMode::Shuffle) {
        if (count == 1)
            return 0;

        if (m_shuffle_bag.is_empty()) {
            // Refill the bag with every index except the currently playing
            // one so we never repeat a track back-to-back.
            for (int i = 0; i < count; ++i) {
                if (current.has_value() && i == *current)
                    continue;
                m_shuffle_bag.append(i);
            }
            // Fisher–Yates shuffle.
            for (size_t i = m_shuffle_bag.size(); i > 1; --i)
                AK::swap(m_shuffle_bag[i - 1], m_shuffle_bag[get_random_uniform(i)]);
        }
        return m_shuffle_bag.take_last();
    }

    int next = current.has_value() ? *current + 1 : 0;
    if (next < count)
        return next;

    // End of the list.
    if (m_mode == PlayMode::RepeatAll)
        return 0;
    return {};
}

Optional<int> PlaylistStore::previous_index(int current) const
{
    if (size() == 0)
        return {};
    int previous = current - 1;
    if (previous < 0)
        previous = 0;
    return previous;
}

bool PlaylistStore::load_m3u8(StringView path)
{
    // Read the file directly; M3UParser::from_memory is byte-oriented and
    // handles UTF-8/GBK content without transcoding.
    auto maybe_file = Core::File::open(path, Core::File::OpenMode::Read);
    if (maybe_file.is_error())
        return false;
    auto maybe_bytes = maybe_file.release_value()->read_until_eof();
    if (maybe_bytes.is_error())
        return false;
    auto bytes = maybe_bytes.release_value();

    bool has_non_whitespace = false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto byte = bytes[i];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
            has_non_whitespace = true;
            break;
        }
    }
    if (!has_non_whitespace)
        return false; // Empty or whitespace-only playlist: nothing to load.

    auto use_utf8 = path.ends_with(".m3u8"sv, AK::CaseSensitivity::CaseInsensitive);
    auto parser = M3UParser::from_memory(ByteString { bytes, NoChomp }, use_utf8);
    auto items = parser->parse(true);
    if (items->size() <= 0)
        return false;

    LexicalPath playlist_path { path };
    auto playlist_dir = playlist_path.dirname();

    Vector<Entry> loaded;
    for (auto& item : *items) {
        Entry entry;
        entry.path = item.path;
        if (!LexicalPath { entry.path }.is_absolute())
            entry.path = ByteString::formatted("{}/{}", playlist_dir, entry.path);
        if (!FileSystem::exists(entry.path))
            continue;
        if (item.extended_info.has_value()) {
            entry.display_title = item.extended_info->track_display_title;
            entry.duration_seconds = item.extended_info->track_length_in_seconds;
            entry.file_size = item.extended_info->file_size_in_bytes.value_or(file_size_or_zero(entry.path));
        } else {
            entry.file_size = file_size_or_zero(entry.path);
        }
        if (!entry.display_title.has_value())
            entry.display_title = LexicalPath::title(entry.path);
        loaded.append(move(entry));
    }

    if (loaded.is_empty())
        return false;

    m_entries = move(loaded);
    m_shuffle_bag.clear();
    on_change();
    return true;
}

bool PlaylistStore::save_m3u8(StringView path) const
{
    StringBuilder builder;
    builder.append("#EXTM3U\n"sv);
    for (auto const& entry : m_entries) {
        auto title = entry.display_title.value_or(LexicalPath::title(entry.path));
        auto duration = static_cast<i64>(entry.duration_seconds.value_or(0));
        builder.appendff("#EXTINF:{},{}\n{}\n", duration, title, entry.path);
    }

    auto maybe_file = Core::File::open(path, Core::File::OpenMode::Write);
    if (maybe_file.is_error())
        return false;
    auto file = maybe_file.release_value();
    auto content = builder.to_byte_string();
    if (file->write_until_depleted(content.bytes()).is_error())
        return false;
    file->close();
    return true;
}

Vector<PlaylistStore::Entry> PlaylistStore::scan_folder(StringView dir, int max_depth)
{
    Vector<Entry> result;

    Function<void(StringView, int)> scan = [&](StringView current_dir, int depth) {
        if (depth > max_depth)
            return;
        Core::DirIterator it(ByteString { current_dir },
            static_cast<Core::DirIterator::Flags>(Core::DirIterator::SkipDots | Core::DirIterator::NoStat));
        while (it.has_next()) {
            auto entry_path = it.next_full_path();
            if (FileSystem::is_directory(entry_path)) {
                scan(entry_path, depth + 1);
                continue;
            }
            if (!is_audio_file(entry_path))
                continue;
            Entry entry;
            entry.path = entry_path;
            entry.display_title = LexicalPath::title(entry_path);
            entry.file_size = file_size_or_zero(entry_path);
            result.append(move(entry));
        }
    };
    scan(dir, 0);

    // Sort by path so the order is stable.
    AK::quick_sort(result, [](Entry const& a, Entry const& b) { return a.path < b.path; });
    return result;
}
