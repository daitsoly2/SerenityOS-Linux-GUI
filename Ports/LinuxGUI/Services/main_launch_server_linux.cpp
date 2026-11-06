/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for LaunchServer. Same story as ConfigServer: the upstream
// main.cpp expects SystemServer to pass in a listening socket, so we bind it
// ourselves on the session portal path.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibIPC/MultiServer.h>
#include <LibMain/Main.h>
#include <LaunchServer/ConnectionFromClient.h>
#include <LaunchServer/Launcher.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/launch"sv));
    TRY(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));

    ::unlink(path.characters());

    auto server = TRY(Core::LocalServer::try_create());
    if (!server->listen(path)) {
        warnln("LaunchServer (Linux port): failed to listen on {}", path);
        return 1;
    }

    auto multi_server = TRY(IPC::MultiServer<LaunchServer::ConnectionFromClient>::try_create(move(server)));

    auto launcher = LaunchServer::Launcher();
    launcher.load_handlers();

    // System-wide and per-user LaunchServer.ini (both optional on the host).
    if (auto system_config = Core::ConfigFile::open_for_system("LaunchServer"); !system_config.is_error())
        launcher.load_config(system_config.release_value());
    if (auto app_config = Core::ConfigFile::open_for_app("LaunchServer"); !app_config.is_error())
        launcher.load_config(app_config.release_value());

    outln("LaunchServer (Linux port): listening on {}", path);
    return event_loop.exec();
}
