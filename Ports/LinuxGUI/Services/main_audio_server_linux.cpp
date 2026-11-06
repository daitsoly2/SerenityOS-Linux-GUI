/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for AudioServer.
//
// Upstream's main.cpp receives already-bound sockets from SystemServer and
// unveils /dev/audio for writing. On the host we listen on the portal paths
// ourselves, and /dev/audio/0 comes from the serenity_compat kernel module
// (an s16le stereo sink, see KernelModules/serenity_compat).
//
// The mixer itself is upstream code and unmodified: it opens /dev/audio/0 and
// writes at real time, which is exactly what the module provides.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibMain/Main.h>

#include <AudioServer/ConnectionFromClient.h>
#include <AudioServer/ConnectionFromManagerClient.h>
#include <AudioServer/Mixer.h>

#include <unistd.h>

static ErrorOr<NonnullRefPtr<Core::LocalServer>> listen_on(StringView path_template)
{
    auto path = TRY(Core::SessionManagement::parse_path_with_sid(path_template));
    TRY(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));
    ::unlink(path.characters());

    auto server = TRY(Core::LocalServer::try_create());
    if (!server->listen(path)) {
        warnln("AudioServer (Linux port): failed to listen on {}", path);
        return Error::from_errno(ECONNREFUSED);
    }
    outln("AudioServer (Linux port): listening on {}", path);
    return server;
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto config = TRY(Core::ConfigFile::open_for_app("Audio", Core::ConfigFile::AllowWriting::Yes));

    // No device is not fatal upstream, and it is not fatal here either: the
    // mixer simply discards audio if the module is not loaded.
    auto mixer = TRY(AudioServer::Mixer::try_create(config));

    auto server = TRY(listen_on("/tmp/session/%sid/portal/audio"sv));
    server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        static int s_next_client_id = 0;
        int client_id = ++s_next_client_id;
        (void)IPC::new_client_connection<AudioServer::ConnectionFromClient>(move(client_socket), client_id, *mixer);
    };

    auto manager_server = TRY(listen_on("/tmp/session/%sid/portal/audiomanager"sv));
    manager_server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        static int s_next_client_id = 0;
        int client_id = ++s_next_client_id;
        (void)IPC::new_client_connection<AudioServer::ConnectionFromManagerClient>(move(client_socket), client_id, *mixer);
    };

    return event_loop.exec();
}
