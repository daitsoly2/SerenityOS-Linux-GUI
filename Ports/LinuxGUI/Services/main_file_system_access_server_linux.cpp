/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for the FileSystemAccessServer.
//
// Like ConfigServer, the SerenityOS main.cpp expects SystemServer to hand over
// a pre-bound socket. There is no SystemServer here, so bind the session portal
// socket ourselves; the path is the one LibFileSystemAccessClient connects to.
//
// Unlike ConfigServer this one is a *real* GUI application: FilePicker shows a
// window, and the very first ConnectionToWindowServer::the() call (from
// expose_window_server_client_id()) runs
//     set_system_theme_from_anonymous_buffer() -> Application::the()->set_system_palette()
// so running without a GUI::Application used to SIGSEGV the server on every
// File/Open request; the asking app then saw the IPC "response" promise fail and
// aborted. Upstream main.cpp creates the Application too, which is why the
// session manager spawns this service *after* WindowServer is up.
//
// NOTE: upstream FileSystemAccessServer::ConnectionFromClient takes only the
// socket (client id is hardcoded to 1 upstream), so we can't use IPC::MultiServer
// here; accept clients manually instead.

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <FileSystemAccessServer/ConnectionFromClient.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibGUI/Application.h>
#include <LibMain/Main.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    // Must exist before any Window/picker code runs (see the comment above).
    auto app = TRY(GUI::Application::create(arguments));
    app->set_quit_when_last_window_deleted(false);

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/filesystemaccess"sv));
    TRY(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));
    ::unlink(path.characters());

    auto server = TRY(Core::LocalServer::try_create());
    if (!server->listen(path)) {
        warnln("FileSystemAccessServer (Linux port): failed to listen on {}", path);
        return 1;
    }
    // Upstream assumes a single client (client id 1); just accept each
    // connection and let ConnectionFromClient self-register.
    server->on_accept = [](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        (void)FileSystemAccessServer::ConnectionFromClient::construct(move(client_socket));
    };

    outln("FileSystemAccessServer (Linux port): listening on {}", path);
    return app->exec();
}
