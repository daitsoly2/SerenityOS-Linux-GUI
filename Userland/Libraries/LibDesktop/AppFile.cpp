/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Spencer Dixon <spencercdixon@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/AppFile.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/Icon.h>
#include <LibGUI/MessageBox.h>
#ifndef AK_OS_SERENITY
#    include <Ports/LinuxGUI/Common/AppIcons.h>
#    include <Ports/LinuxGUI/Common/hostify.h>
#endif

namespace Desktop {

ByteString AppFile::app_file_path_for_app(StringView app_name)
{
    return ByteString::formatted("{}/{}.af", APP_FILES_DIRECTORY, app_name);
}

bool AppFile::exists_for_app(StringView app_name)
{
    return FileSystem::exists(app_file_path_for_app(app_name));
}

NonnullRefPtr<AppFile> AppFile::get_for_app(StringView app_name)
{
    return open(app_file_path_for_app(app_name));
}

NonnullRefPtr<AppFile> AppFile::open(StringView path)
{
    return adopt_ref(*new AppFile(path));
}

void AppFile::for_each(Function<void(NonnullRefPtr<AppFile>)> callback, StringView directory)
{
    Core::DirIterator di(directory, Core::DirIterator::SkipDots);
    if (di.has_error())
        return;
    while (di.has_next()) {
        auto name = di.next_path();
        if (!name.ends_with(".af"sv))
            continue;
        auto path = ByteString::formatted("{}/{}", directory, name);
        auto af = AppFile::open(path);
        if (!af->is_valid())
            continue;
        callback(af);
    }
}

AppFile::AppFile(StringView path)
    : m_config(Core::ConfigFile::open(path).release_value_but_fixme_should_propagate_errors())
    , m_valid(validate())
{
}

bool AppFile::validate() const
{
    if (m_config->read_entry("App", "Name").trim_whitespace().is_empty())
        return false;
    if (m_config->read_entry("App", "Executable").trim_whitespace().is_empty())
        return false;
    return true;
}

ByteString AppFile::name() const
{
    auto name = m_config->read_entry("App", "Name").trim_whitespace().replace("&"sv, ""sv);
    VERIFY(!name.is_empty());
    return name;
}

ByteString AppFile::menu_name() const
{
    auto name = m_config->read_entry("App", "Name").trim_whitespace();
    VERIFY(!name.is_empty());
    return name;
}

ByteString AppFile::executable() const
{
    auto executable = m_config->read_entry("App", "Executable").trim_whitespace();
    VERIFY(!executable.is_empty());
    return executable;
}

Vector<ByteString> AppFile::arguments() const
{
    auto arguments = m_config->read_entry("App", "Arguments").trim_whitespace();
    return arguments.split(' ');
}

ByteString AppFile::description() const
{
    return m_config->read_entry("App", "Description").trim_whitespace();
}

ByteString AppFile::category() const
{
    return m_config->read_entry("App", "Category").trim_whitespace();
}

ByteString AppFile::working_directory() const
{
    return m_config->read_entry("App", "WorkingDirectory").trim_whitespace();
}

ByteString AppFile::icon_path() const
{
    return m_config->read_entry("App", "IconPath").trim_whitespace();
}

#ifndef AK_OS_SERENITY
// Split "GMLPlayground" into { "GML", "Playground" }, keeping acronyms and
// leading digits together ("3DFileViewer" -> { "3D", "File", "Viewer" }).
static Vector<ByteString> split_pascal_case(StringView name)
{
    Vector<ByteString> words;
    StringBuilder current;
    auto flush = [&] {
        if (current.is_empty())
            return;
        words.append(current.to_byte_string());
        current.clear();
    };

    for (size_t i = 0; i < name.length(); ++i) {
        auto ch = name[i];
        bool is_upper = ch >= 'A' && ch <= 'Z';
        if (is_upper && !current.is_empty()) {
            auto previous = name[i - 1];
            bool previous_is_upper = previous >= 'A' && previous <= 'Z';
            bool previous_is_digit = previous >= '0' && previous <= '9';
            bool next_is_lower = i + 1 < name.length() && name[i + 1] >= 'a' && name[i + 1] <= 'z';
            // "WebBrowser" -> Web|Browser, "GMLPlayground" -> GML|Playground,
            // "3DFileViewer" -> 3D|File|Viewer.
            if ((!previous_is_upper && !previous_is_digit) || next_is_lower)
                flush();
        }
        current.append(ch);
    }
    flush();
    return words;
}

static ByteString lowercase(StringView word)
{
    StringBuilder builder;
    for (auto ch : word) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
        builder.append(ch);
    }
    return builder.to_byte_string();
}

// Candidate /res/icons/<size>/ names for an app executable, most specific first.
static Vector<ByteString> icon_name_candidates(StringView app_name)
{
    Vector<ByteString> candidates;
    auto add = [&](ByteString name) {
        if (name.is_empty())
            return;
        for (auto const& existing : candidates) {
            if (existing == name)
                return;
        }
        candidates.append(move(name));
    };

    // "FileManager" -> app-file-manager
    add(ByteString::formatted("app-{}", PortHostify::kebab_case(app_name)));

    auto words = split_pascal_case(app_name);
    StringBuilder joined;
    StringBuilder dashed;
    for (size_t i = 0; i < words.size(); ++i) {
        auto word = lowercase(words[i]);
        joined.append(word);
        if (i != 0)
            dashed.append('-');
        dashed.append(word);
    }
    // "BrickGame" -> app-brickgame, "PDFViewer" -> app-pdfviewer
    add(ByteString::formatted("app-{}", joined.to_byte_string()));
    add(ByteString::formatted("app-{}", dashed.to_byte_string()));

    // Last resort: a single word from the name. Spellings like "WebBrowser" have
    // no app-web-browser.png, but app-browser.png ships with the browser.
    for (auto const& word : words)
        add(ByteString::formatted("app-{}", lowercase(word)));
    return candidates;
}
#endif

GUI::Icon AppFile::icon() const
{
#ifndef AK_OS_SERENITY
    // IconPath= is the Serenity spelling; Icon= is what the Linux port's
    // shortcut writer emits (and what freedesktop files use). Both may be an
    // absolute path, an .ico, a theme name or a Windows executable, which is
    // exactly what the shared resolver understands.
    auto specification = icon_path();
    if (specification.is_empty())
        specification = m_config->read_entry("App", "Icon").trim_whitespace();
    if (!specification.is_empty()) {
        GUI::Icon icon;
        if (auto large = PortIcons::resolve(specification, 32)) {
            icon.set_bitmap_for_size(32, move(large));
            if (auto small = PortIcons::resolve(specification, 16))
                icon.set_bitmap_for_size(16, move(small));
            return icon;
        }
    }
#endif

    auto override_icon = icon_path();
    // FIXME: support pointing to actual .ico files
    if (!override_icon.is_empty())
        return GUI::FileIconProvider::icon_for_path(override_icon);

#ifndef AK_OS_SERENITY
    // "/bin/FileManager" does not exist on the host, so the icon provider has
    // nothing to look at and every app ends up without an icon (start menu,
    // quick launch, taskbar). Map the Serenity path to the icon name that ships
    // in /res/icons instead. Those files were named by hand, so there is no
    // single rule to derive them: "app-file-manager" (FileManager),
    // "app-brickgame" (BrickGame), "app-pdfviewer" (PDFViewer) and
    // "app-browser" (WebBrowser) all coexist. Try the plausible spellings in
    // order instead of guessing one.
    for (auto const& candidate : icon_name_candidates(LexicalPath::basename(executable()))) {
        // try_create_default_icon: default_icon() VERIFYs when the icon is missing,
        // which would abort the shell for every app without an app-*.png.
        if (auto icon_or_error = GUI::Icon::try_create_default_icon(candidate); !icon_or_error.is_error()) {
            if (auto icon = icon_or_error.release_value(); icon.bitmap_for_size(16))
                return icon;
        }
    }
    // 宿主上不要回落到 FileIconProvider("/bin/Xxx")：这个路径不存在，
    // 它会打 "Default icon not found" 然后 VERIFY 崩掉整个 shell。
    return {};
#endif

    return GUI::FileIconProvider::icon_for_path(executable());
}

bool AppFile::run_in_terminal() const
{
    return m_config->read_bool_entry("App", "RunInTerminal", false);
}

bool AppFile::requires_root() const
{
    return m_config->read_bool_entry("App", "RequiresRoot", false);
}

bool AppFile::exclude_from_system_menu() const
{
    return m_config->read_bool_entry("App", "ExcludeFromSystemMenu", false);
}

Vector<ByteString> AppFile::launcher_mime_types() const
{
    Vector<ByteString> mime_types;
    for (auto& entry : m_config->read_entry("Launcher", "MimeTypes").split(',')) {
        entry = entry.trim_whitespace();
        if (!entry.is_empty())
            mime_types.append(entry);
    }
    return mime_types;
}

Vector<ByteString> AppFile::launcher_file_types() const
{
    Vector<ByteString> file_types;
    for (auto& entry : m_config->read_entry("Launcher", "FileTypes").split(',')) {
        entry = entry.trim_whitespace();
        if (!entry.is_empty())
            file_types.append(entry);
    }
    return file_types;
}

Vector<ByteString> AppFile::launcher_protocols() const
{
    Vector<ByteString> protocols;
    for (auto& entry : m_config->read_entry("Launcher", "Protocols").split(',')) {
        entry = entry.trim_whitespace();
        if (!entry.is_empty())
            protocols.append(entry);
    }
    return protocols;
}

ErrorOr<void> AppFile::spawn(ReadonlySpan<StringView> user_arguments) const
{
    if (!is_valid())
        return Error::from_string_literal("AppFile is invalid");

    Vector<StringView> args;

    auto arguments = AppFile::arguments();
    for (auto const& argument : arguments)
        args.append(argument);

    args.extend(Vector(user_arguments));

    TRY(Core::Process::spawn(executable(), args, working_directory()));
    return {};
}

ErrorOr<void> AppFile::spawn_with_escalation(ReadonlySpan<StringView> user_arguments) const
{
    if (!is_valid())
        return Error::from_string_literal("AppFile is invalid");

    StringView exe;
    Vector<StringView, 2> args;

    auto executable = AppFile::executable();
    auto arguments = AppFile::arguments();

    for (auto const& argument : arguments)
        args.append(argument);

    // FIXME: These single quotes won't be enough for executables with single quotes in their name.
    auto pls_with_executable = ByteString::formatted("/bin/pls '{}'", executable);
    if (run_in_terminal() && !requires_root()) {
        exe = "/bin/Terminal"sv;
        args = { "-e"sv, executable };
    } else if (!run_in_terminal() && requires_root()) {
        exe = "/bin/Escalator"sv;
        args = { executable };
    } else if (run_in_terminal() && requires_root()) {
        exe = "/bin/Terminal"sv;
        args = { "-e"sv, pls_with_executable };
    } else {
        exe = executable;
    }
    args.extend(user_arguments);

    TRY(Core::Process::spawn(exe, args.span(),
        working_directory().is_empty() ? Core::StandardPaths::home_directory() : working_directory()));
    return {};
}

void AppFile::spawn_with_escalation_or_show_error(GUI::Window& window, ReadonlySpan<StringView> arguments) const
{
    if (auto result = spawn_with_escalation(arguments); result.is_error())
        GUI::MessageBox::show_error(&window, ByteString::formatted("Failed to spawn {} with escalation: {}", executable(), result.error()));
}

}
