/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for the Clipboard server.
//
// Like ConfigServer, the SerenityOS main.cpp expects SystemServer to hand over
// a pre-bound socket. There is no SystemServer here, so bind the session portal
// socket ourselves; the path is the one LibGUI/Clipboard.cpp connects to.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <Clipboard/ConnectionFromClient.h>
#include <Clipboard/Storage.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibIPC/MultiServer.h>
#include <LibMain/Main.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/clipboard"sv));
    TRY(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));
    ::unlink(path.characters());

    auto server = TRY(Core::LocalServer::try_create());
    if (!server->listen(path)) {
        warnln("Clipboard (Linux port): failed to listen on {}", path);
        return 1;
    }

    auto multi_server = TRY(IPC::MultiServer<Clipboard::ConnectionFromClient>::try_create(move(server)));
    Clipboard::Storage::the().on_content_change = [] {
        Clipboard::ConnectionFromClient::for_each_client([](auto& client) {
            client.notify_about_clipboard_change();
        });
    };

    outln("Clipboard (Linux port): listening on {}", path);
    return event_loop.exec();
}
