/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Vista-style search scope.
//
// The search box is opt-in (DirectoryView only builds a FilteringProxyModel
// once the user actually types something) AND bounded: by default it only
// works while the file manager is inside the user's home directory. Extra
// locations are opt-in too -- the user has to explicitly add them from the
// "搜索范围" command-bar menu, and the list is persisted in FileManager.ini.
//
// This is both a usability boundary (Vista's Libraries / indexed locations)
// and a hard guard against FileSystemModel walking (and inotify-watching)
// the entire filesystem when a filter is active.

#pragma once

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibConfig/Client.h>
#include <LibCore/StandardPaths.h>

namespace FileManager {

class SearchScope final {
public:
    static SearchScope& the()
    {
        static SearchScope instance;
        return instance;
    }

    ByteString home_directory() const { return Core::StandardPaths::home_directory(); }

    // Explicitly added roots (never includes the home directory).
    Vector<ByteString> const& additional_directories() const { return m_additional_directories; }

    // True when `path` is the home directory (or below it), or when it lives
    // inside one of the directories the user explicitly added.
    bool contains(ByteString const& path) const
    {
        auto candidate = normalized(path);
        if (candidate.is_empty())
            return false;
        if (within(candidate, normalized(home_directory())))
            return true;
        for (auto const& root : m_additional_directories) {
            if (within(candidate, root))
                return true;
        }
        return false;
    }

    // Returns true only when a new root was actually added.
    bool add(ByteString const& path)
    {
        auto root = normalized(path);
        if (root.is_empty() || contains(root))
            return false;
        m_additional_directories.append(root);
        persist();
        return true;
    }

    void remove(ByteString const& path)
    {
        auto root = normalized(path);
        m_additional_directories.remove_all_matching([&](auto const& entry) { return entry == root; });
        persist();
    }

    void reload() { load(); }

private:
    SearchScope() { load(); }

    static ByteString normalized(ByteString const& path)
    {
        return LexicalPath::canonicalized_path(path);
    }

    // Prefix compare with a path-separator boundary, so /home/user2 is not
    // accidentally considered part of /home/user.
    static bool within(ByteString const& candidate, ByteString const& root)
    {
        if (root.is_empty())
            return false;
        if (root == "/"sv)
            return true;
        if (candidate == root)
            return true;
        return candidate.starts_with(root)
            && candidate.length() > root.length()
            && candidate[root.length()] == '/';
    }

    void load()
    {
        m_additional_directories.clear();
        // '|' is used as the separator because it is rare in paths and safe in
        // the INI value (unlike ':' or ';' which show up in real paths).
        auto value = Config::read_string("FileManager"sv, "Search"sv, "AdditionalDirectories"sv, ""sv);
        for (auto const& part : value.split('|')) {
            if (part.is_empty())
                continue;
            auto root = normalized(part);
            if (!root.is_empty() && !m_additional_directories.contains_slow(root))
                m_additional_directories.append(root);
        }
    }

    void persist()
    {
        StringBuilder builder;
        bool first = true;
        for (auto const& root : m_additional_directories) {
            if (!first)
                builder.append('|');
            builder.append(root);
            first = false;
        }
        Config::write_string("FileManager"sv, "Search"sv, "AdditionalDirectories"sv, builder.to_byte_string());
    }

    Vector<ByteString> m_additional_directories;
};

}
