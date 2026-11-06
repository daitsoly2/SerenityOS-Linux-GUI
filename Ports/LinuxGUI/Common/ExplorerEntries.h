/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// One entry descriptor shared by the desktop (serenity-shell) and the file
// manager (serenity-file-manager).
//
// The two processes keep their own rendering models: the desktop needs icon
// positions and shortcut metadata, the file manager needs FileSystemModel
// indices. What used to diverge was everything at the *interaction* boundary:
// two sets of "is this an exe / an archive / an image" checks, two launchers,
// two context menus with different wording and different item order. Both now
// convert their selection into ExplorerShared::Entry and hand it to
// ExplorerMenus.h, so a file behaves identically on the desktop and in a
// window, without merging the processes.

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Process.h>
#include <LibGfx/Bitmap.h>
#include <Ports/LinuxGUI/Common/ArchiveOps.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <sys/stat.h>

namespace ExplorerShared {

enum class Kind {
    File,
    Directory,
    WindowsBinary,
    Archive,
    Image,
    Shortcut,
};

struct Entry {
    ByteString path;        // Real file/folder this entry points at.
    ByteString source_file; // .af / .desktop file for shortcuts; empty otherwise.
    ByteString display_name;
    ByteString exec; // Shortcut Exec= (may be an absolute program path).
    bool is_shortcut { false };

    static Entry from_path(ByteString path)
    {
        Entry entry;
        entry.path = move(path);
        entry.display_name = LexicalPath::basename(entry.path);
        return entry;
    }

    static Entry from_shortcut(ByteString name, ByteString exec, ByteString path, ByteString source_file)
    {
        Entry entry;
        entry.display_name = move(name);
        entry.exec = move(exec);
        entry.path = move(path);
        entry.source_file = move(source_file);
        entry.is_shortcut = true;
        return entry;
    }

    ByteString name() const
    {
        if (!display_name.is_empty())
            return display_name;
        return LexicalPath::basename(target_path());
    }

    // The file an action (open/compress/set wallpaper) should operate on.
    // Desktop shortcuts store either Path= or an absolute Exec=.
    ByteString target_path() const
    {
        struct stat st;
        if (!path.is_empty() && stat(path.characters(), &st) == 0)
            return path;
        if (exec.starts_with('/') && stat(exec.characters(), &st) == 0)
            return exec;
        return path;
    }

    // Deleting a shortcut must delete the .desktop/.af, not its target.
    ByteString removal_path() const
    {
        return is_shortcut ? source_file : target_path();
    }

    bool is_directory() const
    {
        struct stat st;
        auto target = target_path();
        return !target.is_empty() && stat(target.characters(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    static bool is_windows_binary(ByteString const& path)
    {
        auto lower = path.to_lowercase();
        for (auto suffix : { ".exe"sv, ".msi"sv, ".bat"sv, ".cmd"sv }) {
            if (lower.ends_with(suffix))
                return true;
        }
        return false;
    }

    static bool is_image(ByteString const& path)
    {
        return Gfx::Bitmap::is_path_a_supported_image_format(path);
    }

    bool is_windows_binary() const { return is_windows_binary(target_path()); }
    bool is_archive() const { return PortArchiveOps::is_archive(target_path()); }
    bool is_image() const { return is_image(name()) || is_image(target_path()); }

    Kind kind() const
    {
        if (is_shortcut)
            return Kind::Shortcut;
        if (is_directory())
            return Kind::Directory;
        if (is_windows_binary())
            return Kind::WindowsBinary;
        if (is_archive())
            return Kind::Archive;
        if (is_image())
            return Kind::Image;
        return Kind::File;
    }
};

enum class WindowsRunMode {
    Auto, // serenity-winapp looks the program up in winapps.ini (managed programs)
    Game, // force umu-run + DWProton
    App,  // force upstream wine
};

// Single launch path for .exe/.msi/.bat/.cmd on the desktop and in the file
// manager. Auto (rather than a hard-coded --app) is important: registered
// Windows programs carry their own mode/prefix, and the launcher resolves it.
inline void launch_windows_binary(ByteString const& path, WindowsRunMode mode)
{
    Vector<ByteString> arguments;
    switch (mode) {
    case WindowsRunMode::Game:
        arguments.append("--game"sv);
        break;
    case WindowsRunMode::App:
        arguments.append("--app"sv);
        break;
    case WindowsRunMode::Auto:
        break;
    }
    arguments.append(path);

    auto launcher = PortHostify::resolve_executable("serenity-winapp"sv);
    auto result = Core::Process::spawn(launcher.view(),
        ReadonlySpan<ByteString> { arguments.data(), arguments.size() },
        LexicalPath::dirname(path));
    if (result.is_error())
        warnln("explorer: failed to launch {} with {}: {}", path, launcher, result.error());
}

}
