/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// GTK3 模块：把 GtkFileChooserDialog 接到 Serenity 原生文件选择器。
//
// 为什么需要它：
//   xdg-desktop-portal 只覆盖 GtkFileChooserNative（GTK4 默认、GTK3 要
//   GTK_USE_PORTAL=1）。gedit 这类老应用用的是 GtkFileChooserDialog —— 一个
//   应用内 widget，压根不走 Native 路径，GTK_USE_PORTAL 对它无效。
//   GTK3 没有「文件选择器插件点」，但有官方的 GTK_MODULES 扩展点；这里用
//   GtkWidget "show" 的 emission hook 抓住所有 FileChooserDialog，把显示换成
//   一次 serenity-file-picker 调用，拿到路径后用 gtk_file_chooser_set_filename()
//   + gtk_dialog_response() 原样喂回应用。应用侧代码一行都不用改。
//
// 为什么不走 D-Bus portal：GTK 的 gtk_dialog_run() 会进嵌套主循环，
// GDBus 的 signal_subscribe / add_filter 在这个上下文里收不到单播 Response
// （AddMatch 装了、消息也到了 daemon，但本地派发不过来）。直接 fork helper
// 既避开了 GTK 源码维护，也避开了 D-Bus 时序；portal 那条路继续服务 Native
// 应用（GTK4 / GTK3 Native），两条路互不干扰。
//
// 构建：g++ -shared -fPIC -o libserenity-filechooser.so this.cpp \
//           $(pkg-config --cflags --libs gtk+-3.0)
// 部署：放到 gtk-3.0/modules/，会话里设 GTK_MODULES=serenity-filechooser。

#include <gtk/gtk.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

char const* const LOG_PATH = "/tmp/serenity-gtk-chooser.log";
char const* const HELPER = "/serenity/bin/serenity-file-picker";
char const* const HELPER_LOG = "/tmp/serenity-gtk-picker-stderr.log";

void log_line(char const* format, ...)
{
    FILE* file = fopen(LOG_PATH, "a");
    if (!file)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

// 同步跑 helper，返回它打印的路径（调用者 g_free）。
char* helper_pick(GtkFileChooser* chooser, gboolean saving)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0)
        return nullptr;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return nullptr;
    }

    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        int err = open(HELPER_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (err >= 0) {
            dup2(err, STDERR_FILENO);
            close(err);
        }

        char* current_name = saving ? gtk_file_chooser_get_current_name(chooser) : nullptr;
        char* start = gtk_file_chooser_get_filename(chooser);
        char title_open[] = "打开文件";
        char title_save[] = "另存为";

        char* argv[12] = {};
        int index = 0;
        argv[index++] = const_cast<char*>("serenity-file-picker");
        argv[index++] = saving ? const_cast<char*>("save") : const_cast<char*>("open");
        argv[index++] = const_cast<char*>("--title");
        argv[index++] = saving ? title_save : title_open;
        if (current_name && *current_name) {
            argv[index++] = const_cast<char*>("--name");
            argv[index++] = current_name;
        }
        if (start && *start) {
            argv[index++] = const_cast<char*>("--start");
            argv[index++] = start;
        }
        argv[index] = nullptr;
        execv(HELPER, argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    char buffer[4096] = {};
    size_t length = 0;
    for (;;) {
        ssize_t count = read(pipe_fds[0], buffer + length, sizeof(buffer) - length - 1);
        if (count > 0) {
            length += (size_t)count;
            if (length >= sizeof(buffer) - 1)
                break;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    close(pipe_fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' || buffer[length - 1] == ' '))
        buffer[--length] = '\0';

    log_line("helper exit=%d path=%s", WIFEXITED(status) ? WEXITSTATUS(status) : -1, buffer);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || length == 0)
        return nullptr;
    return g_strdup(buffer);
}

gboolean on_idle(gpointer userdata)
{
    GtkWidget* widget = GTK_WIDGET(userdata);
    auto* chooser = GTK_FILE_CHOOSER(widget);
    auto action = gtk_file_chooser_get_action(chooser);
    gboolean saving = action == GTK_FILE_CHOOSER_ACTION_SAVE || action == GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER;

    // 不能 hide()：gtk_dialog_run() 看到 dialog 隐藏会直接返回 GTK_RESPONSE_NONE，
    // 后面的 set_filename / response 全白做。保持窗口可见（用户只会看到它一闪而过）。
    log_line("intercept %s chooser", saving ? "save" : "open");
    char* path = helper_pick(chooser, saving);

    log_line("before set_filename / response (main_level=%d)", gtk_main_level());
    if (path) {
        gtk_file_chooser_set_filename(chooser, path);
        log_line("set_filename done");
        gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_ACCEPT);
        log_line("response(ACCEPT) emitted");
        g_free(path);
    } else {
        gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL);
        log_line("response(CANCEL) emitted");
    }
    g_object_unref(widget);
    return G_SOURCE_REMOVE;
}

// "show" 的 emission hook：所有 GtkWidget 都会经过，这里只挑 FileChooserDialog。
gboolean on_show_hook(GSignalInvocationHint*, guint parameter_count, GValue const* parameters, gpointer)
{
    if (parameter_count < 1)
        return TRUE;
    auto* object = static_cast<GObject*>(g_value_get_object(&parameters[0]));
    if (!object || !GTK_IS_FILE_CHOOSER(object) || !GTK_IS_DIALOG(object))
        return TRUE;
    if (g_object_get_data(object, "serenity-chooser-intercepted"))
        return TRUE;
    g_object_set_data(object, "serenity-chooser-intercepted", GINT_TO_POINTER(1));
    // 必须用 timer 而不是 idle：show 是在 gtk_dialog_run() 连接 response handler
    // 之前 emit 的，idle 会在 run 的私有 main loop 启动前就跑掉，那时发出的
    // gtk_dialog_response() 没人收，run() 会永远等（实测 main_level()==0）。
    // 50ms 后 run 一定已经进循环了。
    g_timeout_add(50, on_idle, g_object_ref(object));
    return TRUE;
}

}

extern "C" G_MODULE_EXPORT void gtk_module_init(gint*, gchar***)
{
    // GtkWidget 的信号在 class_init 里注册，而 GTK_MODULES 的 init 跑得很早，
    // 此时 class 还没初始化，g_signal_lookup 会返回 0 —— 先强制 ref 一次。
    g_type_class_ref(GTK_TYPE_WIDGET);
    guint signal_id = g_signal_lookup("show", GTK_TYPE_WIDGET);
    gulong hook_id = signal_id ? g_signal_add_emission_hook(signal_id, 0, on_show_hook, nullptr, nullptr) : 0;
    log_line("module loaded (show signal=%u hook=%lu)", signal_id, hook_id);
}
