/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-file-picker —— 给 xdg-desktop-portal 用的「原生文件选择器」小工具。
//
// 背景：GTK/Firefox/Electron 通过 org.freedesktop.portal.FileChooser 要文件时，
// 只有 GUI 进程才能弹 Serenity 的对话框（GUI::FilePicker 要走 WindowServer 连接，
// 还要把对话框设成请求方窗口的子窗口）。portal 守护本身不是 GUI 进程，所以由它
// fork 本程序：本程序只做一件事 —— 弹出原生对话框，把选中的路径写到 stdout。
//
// 用法（都由 serenity-portal 拼好参数）：
//   serenity-file-picker open [--title T] [--start DIR] [--folder]
//                             [--filter NAME|ext1,ext2 ...]
//   serenity-file-picker save [--title T] [--name BASENAME] [--start DIR]
//
// 退出码：0 = 选中（stdout 一行绝对路径）；1 = 用户取消；2 = 参数错误。
// 对话框关闭后进程立即退出，不给桌面留一个空窗口。

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/StandardPaths.h>
#include <LibGUI/Application.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/Button.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/FileTypeFilter.h>
#include <LibGUI/TextBox.h>
#include <LibMain/Main.h>
#include <stdio.h>

namespace {

void print_usage()
{
    fprintf(stderr,
        "usage: serenity-file-picker open [--title T] [--start DIR] [--folder] [--filter NAME|ext1,ext2]\n"
        "       serenity-file-picker save [--title T] [--name BASENAME] [--start DIR]\n");
}

// "NAME|png,jpg" -> FileTypeFilter { "NAME", { "png", "jpg" } }
// portal 传过来的是 glob（*.png），这里剥掉 "*." 前缀；LibGUI 这一层只认扩展名。
GUI::FileTypeFilter parse_filter(StringView spec)
{
    GUI::FileTypeFilter filter;
    auto separator = spec.find('|');
    filter.name = (separator.has_value() ? spec.substring_view(0, *separator) : spec).to_byte_string();

    Vector<ByteString> extensions;
    if (separator.has_value()) {
        auto list = spec.substring_view(*separator + 1);
        for (auto piece : list.split_view(',')) {
            if (piece.is_empty())
                continue;
            auto extension = piece;
            if (extension.starts_with("*."sv))
                extension = extension.substring_view(2);
            else if (extension.starts_with('.') && extension.length() > 1)
                extension = extension.substring_view(1);
            if (!extension.is_empty())
                extensions.append(extension.to_byte_string());
        }
    }
    if (!extensions.is_empty())
        filter.extensions = move(extensions);
    return filter;
}

ErrorOr<int> pick(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 2;
    }

    ByteString mode { argv[1] };
    if (mode != "open"sv && mode != "save"sv) {
        print_usage();
        return 2;
    }

    ByteString title;
    ByteString start { Core::StandardPaths::home_directory() };
    ByteString name;
    bool folder = false;
    Vector<GUI::FileTypeFilter> filters;

    for (int index = 2; index < argc; ++index) {
        ByteString argument { argv[index] };
        auto next = [&](ByteString& target) -> bool {
            if (index + 1 >= argc)
                return false;
            target = ByteString { argv[++index] };
            return true;
        };
        if (argument == "--title"sv) {
            if (!next(title))
                return 2;
        } else if (argument == "--start"sv) {
            if (!next(start))
                return 2;
        } else if (argument == "--name"sv) {
            if (!next(name))
                return 2;
        } else if (argument == "--folder"sv) {
            folder = true;
        } else if (argument == "--filter"sv) {
            ByteString spec;
            if (!next(spec))
                return 2;
            filters.append(parse_filter(spec));
        } else {
            print_usage();
            return 2;
        }
    }

    // 不用静态 get_open_filepath()：需要拿到 picker 对象，把「文件名框里按回车」
    // 接到确定按钮上（上游 FilePicker 的 filename_textbox 没有 on_return_pressed，
    // 用户手输路径后按回车没反应，看起来像卡住了）。
    auto default_name = ByteString { "Untitled" };
    auto dialog_mode = GUI::FilePicker::Mode::Open;
    if (mode == "save"sv) {
        dialog_mode = GUI::FilePicker::Mode::Save;
        default_name = name.is_empty() ? ByteString { "Untitled" } : name;
    } else if (folder) {
        dialog_mode = GUI::FilePicker::Mode::OpenFolder;
    }

    auto picker = GUI::FilePicker::construct(nullptr, dialog_mode, default_name, start,
        GUI::Dialog::ScreenPosition::Center,
        filters.is_empty() ? Optional<Vector<GUI::FileTypeFilter>> {} : Optional<Vector<GUI::FileTypeFilter>> { move(filters) });
    if (!title.is_empty())
        picker->set_title(title);

    if (auto* filename_box = picker->find_descendant_of_type_named<GUI::TextBox>("filename_textbox")) {
        filename_box->on_return_pressed = [&] {
            if (auto* ok_button = picker->find_descendant_of_type_named<GUI::Button>("ok_button"))
                ok_button->click();
        };
    }

    if (picker->exec() != GUI::Dialog::ExecResult::OK || !picker->selected_file().has_value() || picker->selected_file()->is_empty()) {
        fprintf(stderr, "serenity-file-picker: cancelled\n");
        return 1;
    }

    printf("%s\n", picker->selected_file()->characters());
    fflush(stdout);
    return 0;
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    // 排错用：被 portal 以 posix_spawn 拉起时，只有 stderr（重定向到
    // /tmp/serenity-portal-picker.log）能看到它走到哪一步。
    fprintf(stderr, "serenity-file-picker: start argc=%d\n", arguments.argc);
    fflush(stderr);

    // 测试后门：`--auto-pick PATH` 直接返回该路径，不弹任何窗口。
    // portal 侧用 SERENITY_PORTAL_PICKER 指到一个包装脚本就能在完全不碰鼠标的
    // 情况下验证「请求 → 响应 → 客户端拿到 URI」整条链路（见 HANDOFF §15.2）。
    for (int index = 1; index + 1 < arguments.argc; ++index) {
        if (ByteString { arguments.argv[index] } == "--auto-pick") {
            fprintf(stderr, "serenity-file-picker: auto-pick %s\n", arguments.argv[index + 1]);
            fprintf(stdout, "%s\n", arguments.argv[index + 1]);
            fflush(stdout);
            return 0;
        }
    }
    // GUI::Application 是必须的：FilePicker 是 GUI::Dialog，得挂在 WindowServer
    // 连接上，并且靠事件循环等用户操作。本程序没有主窗口，对话框就是唯一窗口。
    auto app = TRY(GUI::Application::create(arguments));
    fprintf(stderr, "serenity-file-picker: gui connected\n");
    fflush(stderr);
    fprintf(stderr, "serenity-file-picker: showing picker\n");
    fflush(stderr);
    auto result = pick(arguments.argc, arguments.argv);
    fprintf(stderr, "serenity-file-picker: picker returned %d\n", result.is_error() ? -1 : result.value());
    fflush(stderr);
    // 对话框已经关了，别再让事件循环空转。
    app->quit();
    return result;
}
