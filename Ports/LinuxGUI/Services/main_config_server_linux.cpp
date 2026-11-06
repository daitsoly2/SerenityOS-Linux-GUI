/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for ConfigServer.
//
// The SerenityOS main.cpp relies on SystemServer handing over a pre-bound
// socket (IPC::MultiServer::try_create() with no arguments). On the host there
// is no SystemServer, so we create and listen on the socket ourselves, exactly
// like the WindowServer port does.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/System.h>
#include <LibIPC/MultiServer.h>
#include <LibMain/Main.h>
#include <ConfigServer/ConnectionFromClient.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/config"sv));
    TRY(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));

    ::unlink(path.characters());

    auto server = TRY(Core::LocalServer::try_create());
    if (!server->listen(path)) {
        warnln("ConfigServer (Linux port): failed to listen on {}", path);
        return 1;
    }

    auto multi_server = TRY(IPC::MultiServer<ConfigServer::ConnectionFromClient>::try_create(move(server)));
    outln("ConfigServer (Linux port): listening on {}", path);
    return event_loop.exec();
}
