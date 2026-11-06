/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MainWindow.h"
#include "PlayerCore.h"
#include "SkinTheme.h"
#include <LibAudio/ConnectionToServer.h>
#include <LibConfig/Client.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath thread unix proc"));

    StringView file_path;

    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(file_path, "要打开的音频或播放列表文件", "file", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto app = TRY(GUI::Application::create(arguments));
    auto audio_client = TRY(Audio::ConnectionToServer::try_create());

    // GUI::FilePicker reads FileManager/DirectoryView/ShowDotFiles; without
    // that domain ConfigServer disconnects us mid-dialog and the picker dies.
    Config::pledge_domains({ "TTPlayer", "FileManager" });
    app->set_config_domain("TTPlayer"_string);

    // Discover installed skins (built-in + user) and restore the selection.
    SkinTheme::the().rescan();
    SkinTheme::the().load_preference();

    auto core = adopt_ref(*new PlayerCore(audio_client));
    auto window = MainWindow::create(core);

    window->show();

    if (!file_path.is_empty())
        window->play_file_path(file_path.to_byte_string());

    return app->exec();
}
