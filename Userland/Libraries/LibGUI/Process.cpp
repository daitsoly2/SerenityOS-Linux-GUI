/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Process.h>
#include <LibGUI/UITranslation.h>
#include <unistd.h>

template<typename StringType>
void spawn_or_show_error(GUI::Window* parent_window, StringView path, ReadonlySpan<StringType> arguments, StringView working_directory)
{
    auto spawn_result = Core::Process::spawn(path, arguments, working_directory);
    if (!spawn_result.is_error())
        return;

    // ENOENT here means the binary is not part of this build, not that the app
    // crashed: on the Linux port that happens for apps upstream ships but this
    // build skips (SystemMonitor needs LibSymbolication, Help needs LibWeb).
    // Say so instead of surfacing "posix_spawn: No such file or directory".
    if (spawn_result.error().code() == ENOENT)
        GUI::MessageBox::show_error(parent_window,
            ByteString::formatted("{}: {}", GUI::translate_ui("Not installed"sv), path));
    else
        GUI::MessageBox::show_error(parent_window, ByteString::formatted("Failed to spawn {}: {}", path, spawn_result.error()));
}

namespace GUI {

void Process::spawn_or_show_error(Window* parent_window, StringView path, ReadonlySpan<ByteString> arguments, StringView working_directory)
{
    ::spawn_or_show_error<ByteString>(parent_window, path, arguments, working_directory);
}

void Process::spawn_or_show_error(Window* parent_window, StringView path, ReadonlySpan<StringView> arguments, StringView working_directory)
{
    ::spawn_or_show_error<StringView>(parent_window, path, arguments, working_directory);
}

void Process::spawn_or_show_error(Window* parent_window, StringView path, ReadonlySpan<char const*> arguments, StringView working_directory)
{
    ::spawn_or_show_error<char const*>(parent_window, path, arguments, working_directory);
}

}
