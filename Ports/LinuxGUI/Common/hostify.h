/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <unistd.h>

// Host executable lookup for the Linux GUI port.
//
// Serenity apps name themselves with Serenity-internal paths ("/bin/Calculator").
// On the host the same program is installed as "serenity-<kebab-case>", next to
// the rest of the port: $SERENITY_BIN first (exported by serenitid), then
// /usr/local/bin, then whatever $PATH finds.
//
// The conversion used to be copy-pasted in four places (LibCore/Process.cpp,
// LaunchServer/Launcher.cpp, Taskbar/main.cpp and DesktopIcons/desktop_icons.cpp).
// The shell merge made the copies drift apart, so every caller goes through this
// header now: one rule, one place to change.

namespace PortHostify {

// Where the port installs its runtime (bin/, lib/, res/, share/). Keeping it in
// one directory means the desktop can be moved or removed as a unit instead of
// being smeared over /usr/local. serenitid exports SERENITY_PREFIX.
inline ByteString install_prefix()
{
    if (char const* prefix = getenv("SERENITY_PREFIX"); prefix && *prefix)
        return ByteString { prefix };
    return ByteString { "/serenity" };
}

// "FileManager" -> "file-manager", "Calculator" -> "calculator".
inline ByteString kebab_case(StringView pascal_case)
{
    StringBuilder builder;
    for (size_t i = 0; i < pascal_case.length(); ++i) {
        auto ch = pascal_case[i];
        if (ch >= 'A' && ch <= 'Z') {
            if (i != 0)
                builder.append('-');
            builder.append(static_cast<char>(ch - 'A' + 'a'));
        } else {
            builder.append(ch);
        }
    }
    return builder.to_byte_string();
}

// Look `name` up in $PATH. Returns {} when it is not installed.
inline ByteString executable_on_path(StringView name)
{
    char const* path_env = getenv("PATH");
    if (!path_env || !*path_env)
        path_env = "/usr/local/bin:/usr/bin:/bin";

    ByteString path_string { path_env };
    for (auto directory : path_string.split_view(':')) {
        if (directory.is_empty())
            continue;
        auto candidate = ByteString::formatted("{}/{}", directory, name);
        if (access(candidate.characters(), X_OK) == 0)
            return candidate;
    }
    return {};
}

// "/bin/FileManager" -> "/usr/local/bin/serenity-file-manager".
//
// Paths that are not Serenity app paths, and paths whose host counterpart is not
// installed, are returned unchanged so callers can report ENOENT themselves.
inline ByteString hostify_executable(ByteString const& executable)
{
    if (!executable.starts_with("/bin/"sv) && !executable.starts_with("/usr/bin/"sv))
        return executable;

    auto host_name = ByteString::formatted("serenity-{}", kebab_case(LexicalPath::basename(executable)));

    if (char const* bin = getenv("SERENITY_BIN"); bin && *bin) {
        auto candidate = ByteString::formatted("{}/{}", bin, host_name);
        if (access(candidate.characters(), X_OK) == 0)
            return candidate;
    }
    auto prefix_candidate = ByteString::formatted("{}/bin/{}", install_prefix(), host_name);
    if (access(prefix_candidate.characters(), X_OK) == 0)
        return prefix_candidate;
    auto system = ByteString::formatted("/usr/local/bin/{}", host_name);
    if (access(system.characters(), X_OK) == 0)
        return system;
    if (auto found = executable_on_path(host_name); !found.is_empty())
        return found;

    return executable;
}

// Resolve an Exec= value from a shortcut: a Serenity path (see above), an
// absolute host path, or a bare program name looked up in $SERENITY_BIN / $PATH.
inline ByteString resolve_executable(ByteString const& executable)
{
    if (executable.contains('/'))
        return hostify_executable(executable);

    if (char const* bin = getenv("SERENITY_BIN"); bin && *bin) {
        auto candidate = ByteString::formatted("{}/{}", bin, executable);
        if (access(candidate.characters(), X_OK) == 0)
            return candidate;
    }
    auto prefix_candidate = ByteString::formatted("{}/bin/{}", install_prefix(), executable);
    if (access(prefix_candidate.characters(), X_OK) == 0)
        return prefix_candidate;
    if (auto found = executable_on_path(executable); !found.is_empty())
        return found;

    return executable;
}

// Locate the first of `candidates` that is installed: bare names go through
// $PATH, paths through access() directly. Returns {} when none is present.
inline ByteString find_tool(Vector<ByteString> const& candidates)
{
    for (auto const& candidate : candidates) {
        if (candidate.contains('/')) {
            if (access(candidate.characters(), X_OK) == 0)
                return candidate;
            continue;
        }
        if (auto found = executable_on_path(candidate); !found.is_empty())
            return found;
    }
    return {};
}

}
