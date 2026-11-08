/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Unified, in-memory shortcut model for the Linux GUI port.
//
// The desktop, the start menu and the file manager used to parse .af and
// freedesktop .desktop files with separate code and separate rules. That is
// why filtering (NoDisplay / Terminal / console-only), localized names and
// icon lookup all behaved differently depending on where an entry appeared.
//
// Everything now normalizes into ShortcutRecord at scan time. A freedesktop
// .desktop file is parsed in memory once, converted to the native .af shape and
// the result is *saved* in a managed cache. Subsequent scans read the cached
// .af directly (guarded by a source mtime/size stamp), so already-converted
// launchers are never converted a second time. The original .desktop stays
// untouched - no user file is rewritten or deleted - and the UI only ever sees
// one shape. This also makes it possible to filter non-GUI Linux apps out of
// the Start menu without touching the source launcher.

#include "AppIcons.h"
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <sys/stat.h>

namespace PortShortcuts {

struct Record;
inline bool belongs_in_start_menu(Record const&);

enum class Origin {
    SerenityAppFile,
    FreedesktopDesktop,
};

struct Record {
    Origin origin { Origin::SerenityAppFile };
    ByteString name;              // localized display name
    ByteString latin_name;        // untranslated Name
    ByteString program;           // first token of Exec / Executable
    Vector<ByteString> arguments; // remaining tokens, field codes removed
    ByteString working_directory; // .desktop Path= / .af WorkingDirectory=
    ByteString icon_spec;         // Icon / IconPath / IconData value
    ByteString source_file;
    ByteString category;          // .af Category= or first XDG category
    ByteString categories;        // raw XDG Categories=
    ByteString type;              // .desktop Type=
    ByteString cache_file;        // converted .af (empty for native .af sources)
    ByteString source_stamp;      // mtime:size of the .desktop at conversion time
    bool converted { false };     // true when loaded from the converted .af cache
    bool terminal { false };
    bool no_display { false };
    bool hidden { false };
    bool exclude_from_menu { false };

    ByteString id() const { return LexicalPath::basename(source_file); }
};

// Exec lines can be quoted and may start with env assignments or an `env`
// wrapper (flatpak, wine wrappers). Field codes (%U, %f, ...) are dropped.
inline void split_command(ByteString command, ByteString& program, Vector<ByteString>& arguments)
{
    if (auto percent = command.find('%'); percent.has_value())
        command = command.substring(0, percent.value()).trim_whitespace();

    Vector<ByteString> tokens;
    StringBuilder current;
    char quote = 0;
    for (auto ch : command) {
        if (quote) {
            if (ch == quote) {
                quote = 0;
            } else {
                current.append(ch);
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            if (!current.is_empty()) {
                tokens.append(current.to_byte_string());
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.is_empty())
        tokens.append(current.to_byte_string());

    size_t first = 0;
    while (first < tokens.size()) {
        auto const& token = tokens[first];
        bool assignment = token.contains('=') && !token.starts_with('/') && !token.starts_with('.');
        if (token == "env"sv || assignment) {
            ++first;
            continue;
        }
        break;
    }
    if (first >= tokens.size())
        return;
    program = tokens[first];
    for (size_t i = first + 1; i < tokens.size(); ++i)
        arguments.append(tokens[i]);
}

inline ByteString localized_name(Core::ConfigFile const& config, StringView group)
{
    for (auto const& key : { "Name[zh_CN]"sv, "Name[zh]"sv }) {
        auto name = config.read_entry(group, key).trim_whitespace();
        if (!name.is_empty())
            return name;
    }
    return config.read_entry(group, "Name").trim_whitespace();
}

inline Optional<Record> parse(StringView path)
{
    ByteString path_string { path };
    auto config_or_error = Core::ConfigFile::open(path_string);
    if (config_or_error.is_error())
        return {};
    auto const& config = *config_or_error.value();

    Record record;
    record.source_file = path_string;
    if (path_string.ends_with(".af"sv)) {
        if (!config.has_group("App"))
            return {};
        record.origin = Origin::SerenityAppFile;
        record.name = localized_name(config, "App"sv);
        record.latin_name = record.name;
        record.category = config.read_entry("App", "Category").trim_whitespace();
        record.exclude_from_menu = config.read_bool_entry("App", "ExcludeFromSystemMenu", false);
        record.icon_spec = config.read_entry("App", "Icon").trim_whitespace();
        if (record.icon_spec.is_empty())
            record.icon_spec = config.read_entry("App", "IconPath").trim_whitespace();
        if (record.icon_spec.is_empty())
            record.icon_spec = config.read_entry("App", "IconName").trim_whitespace();
        if (record.icon_spec.is_empty()) {
            auto embedded = config.read_entry("App", "IconData").trim_whitespace();
            if (!embedded.is_empty())
                record.icon_spec = ByteString::formatted("base64:{}", embedded);
        }
        record.working_directory = config.read_entry("App", "WorkingDirectory").trim_whitespace();
        auto command = config.read_entry("App", "Executable").trim_whitespace();
        for (auto const& argument : config.read_entry("App", "Arguments").split(' ')) {
            auto trimmed = argument.trim_whitespace();
            if (!trimmed.is_empty())
                record.arguments.append(trimmed);
        }
        split_command(command, record.program, record.arguments);
    } else if (path_string.ends_with(".desktop"sv)) {
        if (!config.has_group("Desktop Entry"))
            return {};
        record.origin = Origin::FreedesktopDesktop;
        record.name = localized_name(config, "Desktop Entry"sv);
        record.latin_name = config.read_entry("Desktop Entry", "Name").trim_whitespace();
        record.type = config.read_entry("Desktop Entry", "Type", "Application").trim_whitespace();
        record.terminal = config.read_bool_entry("Desktop Entry", "Terminal", false);
        record.no_display = config.read_bool_entry("Desktop Entry", "NoDisplay", false);
        record.hidden = config.read_bool_entry("Desktop Entry", "Hidden", false);
        record.categories = config.read_entry("Desktop Entry", "Categories").trim_whitespace();
        for (auto part : record.categories.split_view(';')) {
            auto category = part.trim_whitespace();
            if (!category.is_empty()) {
                record.category = category.to_byte_string();
                break;
            }
        }
        record.icon_spec = config.read_entry("Desktop Entry", "Icon").trim_whitespace();
        record.working_directory = config.read_entry("Desktop Entry", "Path").trim_whitespace();
        // NotShowIn=serenity is an explicit opt-out. OnlyShowIn is deliberately
        // ignored: we are not one of the desktops it lists, and honouring it
        // would hide most third-party launchers.
        if (config.read_entry("Desktop Entry", "NotShowIn").contains("serenity"sv))
            return {};
        auto command = config.read_entry("Desktop Entry", "Exec").trim_whitespace();
        split_command(command, record.program, record.arguments);
    } else {
        return {};
    }

    if (record.name.is_empty() || record.program.is_empty())
        return {};
    return record;
}

// ---------------------------------------------------------------------------
// Conversion cache: .desktop -> .af, once per source file.
//
// The parsed Record is written back as a native .af so every later reader sees
// the exact same shape. A [Conversion] group records the source path and its
// mtime:size stamp; if those still match, the cached .af is loaded directly and
// the .desktop is not parsed at all. Source launchers are never modified.

inline ByteString conversion_cache_directory()
{
    auto home = Core::StandardPaths::home_directory();
    if (home.is_empty())
        home = "/tmp";
    return ByteString::formatted("{}/.local/share/serenity/shortcuts", home);
}

inline ByteString source_stamp(StringView path)
{
    struct stat info {};
    if (::stat(ByteString { path }.characters(), &info) != 0)
        return "missing"sv;
    return ByteString::formatted("{}:{}", static_cast<long long>(info.st_mtime), static_cast<long long>(info.st_size));
}

inline ByteString cache_path_for(StringView source)
{
    return ByteString::formatted("{}/{:x}.af", conversion_cache_directory(),
        PortIcons::hash_string(source));
}

// Values in an INI file cannot contain newlines; strip them defensively.
inline ByteString ini_value(StringView value)
{
    return ByteString { value }.replace("\n"sv, " "sv).replace("\r"sv, " "sv);
}

inline bool write_converted_af(Record const& record, ByteString const& source,
    ByteString const& stamp, ByteString const& destination)
{
    if (destination.is_empty())
        return false;
    if (Core::Directory::create(conversion_cache_directory(), Core::Directory::CreateDirectories::Yes).is_error())
        return false;

    // Freeze the icon to a concrete file at conversion time: afterwards the
    // cached .af no longer depends on the icon theme being installed.
    auto resolved_icon = PortIcons::resolved_path(record.icon_spec, 32);
    auto icon_value = resolved_icon.is_empty() ? record.icon_spec : resolved_icon;

    StringBuilder arguments;
    for (size_t i = 0; i < record.arguments.size(); ++i) {
        if (i)
            arguments.append(' ');
        arguments.append(record.arguments[i]);
    }

    StringBuilder content;
    content.append("[App]\n"sv);
    content.appendff("Name={}\n", ini_value(record.name));
    content.appendff("Executable={}\n", ini_value(record.program));
    content.appendff("Arguments={}\n", ini_value(arguments.string_view()));
    content.appendff("Icon={}\n", ini_value(icon_value));
    content.appendff("IconName={}\n", ini_value(record.icon_spec));
    content.appendff("Category={}\n", ini_value(record.category));
    content.appendff("WorkingDirectory={}\n", ini_value(record.working_directory));
    content.appendff("ExcludeFromSystemMenu={}\n", belongs_in_start_menu(record) ? "false" : "true");
    content.append("[Conversion]\n"sv);
    content.appendff("Source={}\n", ini_value(source));
    content.appendff("SourceStamp={}\n", ini_value(stamp));
    content.appendff("Type={}\n", ini_value(record.type));
    content.appendff("Terminal={}\n", record.terminal ? "true" : "false");
    content.appendff("NoDisplay={}\n", record.no_display ? "true" : "false");
    content.appendff("Hidden={}\n", record.hidden ? "true" : "false");
    content.appendff("Categories={}\n", ini_value(record.categories));
    content.append("Converter=serenity-app-shortcuts/1\n"sv);

    auto file = Core::File::open(destination,
        Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return false;
    if (file.value()->write_until_depleted(content.string_view().bytes()).is_error())
        return false;
    return true;
}

inline Optional<Record> load_or_convert(StringView desktop_path)
{
    ByteString source { desktop_path };
    auto stamp = source_stamp(source);
    auto cache_path = cache_path_for(source);

    if (FileSystem::is_regular_file(cache_path)) {
        auto metadata = Core::ConfigFile::open(cache_path);
        if (!metadata.is_error()
            && metadata.value()->read_entry("Conversion", "Source") == source
            && metadata.value()->read_entry("Conversion", "SourceStamp") == stamp) {
            auto cached = parse(cache_path);
            if (cached.has_value()) {
                auto const& group = *metadata.value();
                cached->origin = Origin::FreedesktopDesktop;
                cached->source_file = source;
                cached->cache_file = cache_path;
                cached->source_stamp = stamp;
                cached->converted = true;
                cached->type = group.read_entry("Conversion", "Type", "Application");
                cached->terminal = group.read_bool_entry("Conversion", "Terminal", false);
                cached->no_display = group.read_bool_entry("Conversion", "NoDisplay", false);
                cached->hidden = group.read_bool_entry("Conversion", "Hidden", false);
                cached->categories = group.read_entry("Conversion", "Categories");
                return cached;
            }
        }
    }

    auto record = parse(source);
    if (!record.has_value())
        return {};
    if (write_converted_af(*record, source, stamp, cache_path)) {
        record->cache_file = cache_path;
        record->source_stamp = stamp;
    }
    return record;
}

// Scan directories in order; a later directory overrides an earlier entry with
// the same file name (system defaults first, user's Desktop last).
// Native .af files are read directly; .desktop files go through the conversion
// cache so an already-converted launcher is never converted a second time.
inline Vector<Record> scan(Vector<ByteString> const& directories)
{
    Vector<Record> records;
    HashMap<ByteString, size_t> seen;
    for (auto const& directory : directories) {
        Core::DirIterator it { directory, Core::DirIterator::SkipDots };
        if (it.has_error())
            continue;
        while (it.has_next()) {
            auto name = it.next_path();
            auto path = ByteString::formatted("{}/{}", directory, name);
            Optional<Record> record;
            ByteString key;
            if (name.ends_with(".af"sv)) {
                record = parse(path);
                key = name;
            } else if (name.ends_with(".desktop"sv)) {
                record = load_or_convert(path);
                // A .desktop and a converted .af for the same launcher must not
                // both appear on the desktop; key them by the same stem.
                key = ByteString::formatted("{}.af", LexicalPath { name }.title());
            } else {
                continue;
            }
            if (!record.has_value())
                continue;
            if (auto found = seen.find(key); found != seen.end()) {
                records[found->value] = record.release_value();
                continue;
            }
            seen.set(key, records.size());
            records.append(record.release_value());
        }
    }
    return records;
}

inline Vector<ByteString> xdg_application_directories()
{
    Vector<ByteString> directories;
    auto home = Core::StandardPaths::home_directory();
    if (!home.is_empty()) {
        directories.append(ByteString::formatted("{}/.local/share/applications", home));
        directories.append(ByteString::formatted("{}/.local/share/flatpak/exports/share/applications", home));
    }
    directories.append(ByteString { "/var/lib/flatpak/exports/share/applications" });
    directories.append(ByteString { "/usr/local/share/applications" });
    directories.append(ByteString { "/usr/share/applications" });
    return directories;
}

// Start-menu filtering. Beyond the spec flags, a lot of distro .desktop files
// declare Terminal=false but are still console tools / setup helpers with no
// window. Those are filtered by XDG category and by a user-editable blocklist:
//
//   ~/.config/serenity/app-filter.ini
//   [Blocked]
//   display-im7.q16=true
//   some.desktop=true
//
inline bool is_blocked_by_user(Record const& record)
{
    auto home = Core::StandardPaths::home_directory();
    if (home.is_empty())
        return false;
    auto config = Core::ConfigFile::open(ByteString::formatted("{}/.config/serenity/app-filter.ini", home));
    if (config.is_error())
        return false;
    auto const& file = *config.value();
    for (auto key : { record.id(), LexicalPath::basename(record.program), record.program }) {
        if (!key.is_empty() && file.read_bool_entry("Blocked", key, false))
            return true;
    }
    return false;
}

inline bool belongs_in_start_menu(Record const& record)
{
    if (record.hidden || record.no_display || record.exclude_from_menu || record.terminal)
        return false;
    if (!record.type.is_empty() && record.type != "Application"sv)
        return false;
    auto categories = record.categories;
    for (auto non_gui : { "ConsoleOnly"sv, "TerminalEmulator"sv, "Building"sv, "Debugger"sv }) {
        if (categories.contains(non_gui))
            return false;
    }
    // Console helpers that forgot Terminal=true / ConsoleOnly.
    static constexpr StringView known_non_gui[] = {
        "display-im7.q16"sv, "bluetooth-sendto"sv, "xdg-open"sv,
        "gnome-terminal"sv, "xterm"sv, "update-manager"sv,
    };
    auto program = LexicalPath::basename(record.program);
    for (auto entry : known_non_gui) {
        if (program == entry)
            return false;
    }
    return !is_blocked_by_user(record);
}

// Icon for a shortcut *file* as seen by FileManager / the desktop.
inline RefPtr<Gfx::Bitmap const> icon_for_record(Record const& record, int size)
{
    return PortIcons::for_shortcut(record.icon_spec, record.program, size);
}

inline RefPtr<Gfx::Bitmap const> icon_for_path(StringView path, int size)
{
    auto record = parse(path);
    if (!record.has_value())
        return {};
    return icon_for_record(*record, size);
}

inline bool is_shortcut_or_executable(StringView path)
{
    ByteString value { path };
    return PortIcons::is_windows_executable(path)
        || value.ends_with(".af"sv) || value.ends_with(".desktop"sv);
}

// Single entry point for GUI::FileIconProvider's host-only branch.
inline RefPtr<Gfx::Bitmap const> icon_for_file(StringView path, int size)
{
    if (PortIcons::is_windows_executable(path))
        return PortIcons::from_executable(path, size);
    ByteString path_string { path };
    if (path_string.ends_with(".af"sv) || path_string.ends_with(".desktop"sv))
        return icon_for_path(path, size);
    return {};
}

}
