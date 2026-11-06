/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <AK/Try.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <LibTimeZone/TimeZone.h>
#include <sys/wait.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio rpath wpath cpath"));
    TRY(Core::System::unveil("/etc/timezone", "rwc"));
    TRY(Core::System::unveil(nullptr, nullptr));

    StringView time_zone;
    bool list_time_zones = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(list_time_zones, "List all available time zones", "list-time-zones", 'l');
    args_parser.add_positional_argument(time_zone, "The time zone to set", "time-zone", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (list_time_zones) {
        for (auto time_zone : TimeZone::all_time_zones())
            outln("{}", time_zone.name);
        return 0;
    }

    if (time_zone.is_empty()) {
        outln("{}", TimeZone::system_time_zone());
        return 0;
    }

#if defined(AK_OS_SERENITY)
    TRY(TimeZone::change_time_zone(time_zone));
#else
    // Linux host: LibTimeZone::change_time_zone is deliberately a no-op here
    // (it refuses to touch the host's /etc), so persist the setting through
    // timedatectl instead — the same thing GNOME/KDE settings call. For an
    // unprivileged session this needs a polkit rule (or an already-trusted
    // active session), otherwise timedatectl exits non-zero.
    bool is_known_time_zone = false;
    for (auto const& known : TimeZone::all_time_zones()) {
        if (known.name == time_zone) {
            is_known_time_zone = true;
            break;
        }
    }
    if (!is_known_time_zone)
        return Error::from_string_literal("Provided time zone is not supported");

    auto pid = TRY(Core::Process::spawn(
        "timedatectl"sv,
        Vector<StringView> { "set-timezone"sv, time_zone }.span(),
        {},
        Core::Process::KeepAsChild::No));
    auto wait_result = TRY(Core::System::waitpid(pid, 0));
    if (!WIFEXITED(wait_result.status) || WEXITSTATUS(wait_result.status) != 0)
        return Error::from_string_literal("timedatectl set-timezone failed (polkit denied the request?)");
#endif
    return 0;
}
