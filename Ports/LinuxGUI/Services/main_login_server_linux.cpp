/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host entry point for the graphical login.
//
// Upstream LoginServer exec()s /bin/SystemServer --user after authenticating,
// which doesn't exist here. This variant keeps the same LoginWindow (GML and
// all) and hands control to the desktop session script instead.
//
//   serenity-login-server                # prompt
//   serenity-login-server --auto-login user
//
// The user still has to exist in /etc/passwd and authenticate against
// /etc/shadow via Core::Account, so the login is real, not decorative.

#include <AK/ByteString.h>
#include <LibCore/Account.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SecretString.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Palette.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <Services/LoginServer/LoginWindow.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// /etc/serenity/session.conf:  [Session] Autologin=serenity
ByteString configured_autologin()
{
    auto config = Core::ConfigFile::open(ByteString { "/etc/serenity/session.conf" });
    if (config.is_error())
        return {};
    auto value = config.value()->read_entry("Session", "Autologin");
    if (value == "false"sv || value == "no"sv || value == "0"sv)
        return {};
    return value;
}

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    ByteString auto_login;

    for (int i = 1; i < arguments.argc; ++i) {
        StringView arg { arguments.argv[i], strlen(arguments.argv[i]) };
        if (arg == "--auto-login"sv && i + 1 < arguments.argc)
            auto_login = ByteString { arguments.argv[++i] };
        else if (arg == "--help"sv) {
            outln("usage: serenity-login-server [--auto-login USER]");
            return 0;
        }
    }

    if (auto_login.is_empty())
        auto_login = configured_autologin();

    if (!auto_login.is_empty()) {
        outln("login: auto-login for '{}'", auto_login);
        // Exit 0 immediately: serenitid starts the desktop shell.
        return 0;
    }

    auto window = LoginServer::LoginWindow::construct();
    window->on_submit = [&] {
        auto username = window->username();
        auto password = Core::SecretString::take_ownership(window->password().to_byte_buffer());
        window->set_password(""sv);

        auto fail_message = "用户名或密码不正确"sv;

        auto account = Core::Account::from_name(username);
        if (account.is_error()) {
            window->set_fail_message(fail_message);
            warnln("login: no such user '{}'", username);
            return;
        }
        if (!account.value().authenticate(password)) {
            window->set_fail_message(fail_message);
            warnln("login: bad password for '{}'", username);
            return;
        }

        outln("login: '{}' authenticated", username);
        window->hide();
        // Exit 0 so serenitid knows the login succeeded and starts the desktop.
        Core::EventLoop::current().quit(0);
    };

    window->show();
    return app->exec();
}
