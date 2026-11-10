/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// libserenity-gtk3-dialog.so —— 定制 GTK3 运行时：把 GtkFileChooserDialog
// 重定向到 Serenity 原生文件选择器。
//
// 为什么需要它：gedit 一类的老 GTK3 应用用的是 GtkFileChooserDialog（应用内
// widget），不是 GtkFileChooserNative。xdg-desktop-portal 只覆盖 Native，
// GTK_USE_PORTAL=1 对这类 dialog 完全无效。这套系统不跟随上游 GTK，所以
// 直接在符号层定制：LD_PRELOAD 覆盖 GTK3 显示对话框的两个入口，凡是文件
// 选择器 dialog 一律改弹原生 FilePicker，把选中路径喂回 chooser，再发出
// 该 dialog 自己的「接受」response。
//
// 等价于在 gtk/gtkdialog.c 与 gtk/gtkwindow.c 里加分支，但不用 fork 整个
// GTK3 源码树、不用改系统 libgtk-3.so.0、不用跟上游 rebase、随时可回滚。
//
// 两个入口（实测 gedit 48.1 只走第二个）：
//   1. gtk_dialog_run()          同步式：run 私有主循环 + 返回 response id
//   2. gtk_window_present()      异步式：show 后靠 "response" 信号回调
// 第 2 种必须等 GTK 主循环转起来再发 response，所以用 g_idle_add 延后一拍；
// 绝不能在 run() 的私有循环外用 gtk_dialog_response()，那是 §16 里挂死的根因。
//
// 零依赖：只链 libc/libdl，所有 GLib/GTK 符号运行时 dlsym 解析。因此这个 .so
// 可以安全注入到所有进程（包括完全不用 GTK 的 Serenity 原生应用），不会产生
// libgtk-3.so.0 这样的 DT_NEEDED。
//
// 开关：
//   SERENITY_NATIVE_FILE_CHOOSER=1  生效（由 serenitid 注入）；未设置 / =0
//                                   时全部直接转发真实 GTK 函数，行为不变。
//   SERENITY_FILE_PICKER=<path>     覆盖 helper 路径（调试用）。
//   SERENITY_GTK3_DIALOG_LOG=<path> 日志路径，缺省 /tmp/serenity-gtk3-dialog.log。

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

/* GTK3 固定枚举值 —— 不 include gtk.h，避免 DT_NEEDED libgtk-3.so.0 */
enum {
    RESP_NONE = -1,
    RESP_ACCEPT = -3,
    RESP_DELETE_EVENT = -4,
    RESP_OK = -5,
    RESP_CANCEL = -6,
    RESP_YES = -8,
    RESP_APPLY = -10,
};
enum { FC_OPEN = 0, FC_SAVE = 1, FC_SELECT_FOLDER = 2, FC_CREATE_FOLDER = 3 };

typedef unsigned long gtype;
typedef void* gptr;
typedef int (*fn_real_run)(gptr);
typedef void (*fn_widget_void)(gptr);
typedef void (*fn_dialog_response)(gptr, int);
typedef gptr (*fn_object_ref)(gptr);
typedef void (*fn_object_unref)(gptr);
typedef unsigned (*fn_idle_add)(int (*)(gptr), gptr);
typedef gtype (*fn_get_type)(void);
typedef int (*fn_is_a)(gptr, gtype);
typedef const char* (*fn_type_name)(gtype);
typedef char* (*fn_get_string)(gptr);
typedef const char* (*fn_get_cstring)(gptr);
typedef void (*fn_set_string)(gptr, const char*);
typedef void (*fn_free)(gptr);
typedef int (*fn_get_action)(gptr);
typedef gptr (*fn_get_widget_for_response)(gptr, int);
typedef int (*fn_get_response_for_widget)(gptr, gptr);
typedef gptr (*fn_get_default_widget)(gptr);
typedef gptr (*fn_container_get_children)(gptr);
typedef void (*fn_set_data)(gptr, const char*, gptr);
typedef gptr (*fn_get_data)(gptr, const char*);

struct glist {
    void* data;
    struct glist* next;
    struct glist* prev;
};

static void log_line(char const* fmt, ...)
{
    char const* path = getenv("SERENITY_GTK3_DIALOG_LOG");
    FILE* f = fopen(path && *path ? path : "/tmp/serenity-gtk3-dialog.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void* sym(char const* name)
{
    return dlsym(RTLD_DEFAULT, name);
}

static int enabled(void)
{
    char const* value = getenv("SERENITY_NATIVE_FILE_CHOOSER");
    return value && strcmp(value, "0") != 0;
}

/* 判断 GObject 实例是否实现某个 GTK 类型/接口 */
static int implements(gptr object, char const* get_type_symbol)
{
    fn_get_type get_type = (fn_get_type)sym(get_type_symbol);
    fn_is_a is_a = (fn_is_a)sym("g_type_check_instance_is_a");
    if (!object || !get_type || !is_a)
        return 0;
    return is_a(object, get_type()) != 0;
}

/* 仅用于日志：GObject 第一个字段是 GTypeClass*，其第一个字段是 GType */
static char const* type_name(gptr object)
{
    fn_type_name name_of = (fn_type_name)sym("g_type_name");
    if (!name_of || !object)
        return "?";
    void** instance = (void**)object;
    if (!instance[0])
        return "?";
    char const* name = name_of(*(gtype*)instance[0]);
    return name ? name : "?";
}

/* 应用可能把 GtkFileChooserWidget 嵌在普通 GtkDialog 里 —— 向下递归找 */
static gptr find_chooser(gptr widget, int depth)
{
    if (!widget || depth > 12)
        return 0;
    if (implements(widget, "gtk_file_chooser_get_type"))
        return widget;
    if (!implements(widget, "gtk_container_get_type"))
        return 0;
    fn_container_get_children children_of = (fn_container_get_children)sym("gtk_container_get_children");
    if (!children_of)
        return 0;
    struct glist* children = (struct glist*)children_of(widget);
    gptr found = 0;
    for (struct glist* item = children; item && !found; item = item->next)
        found = find_chooser(item->data, depth + 1);
    fn_free free_list = (fn_free)sym("g_list_free");
    if (free_list && children)
        free_list(children);
    return found;
}

/* 同一个 dialog 可能被 present 多次（窗口切换），只处理一次 */
static int already_handled(gptr dialog)
{
    fn_get_data get_data = (fn_get_data)sym("g_object_get_data");
    fn_set_data set_data = (fn_set_data)sym("g_object_set_data");
    if (!get_data || !set_data)
        return 0;
    if (get_data(dialog, "serenity-native-chooser"))
        return 1;
    set_data(dialog, "serenity-native-chooser", (gptr)1);
    return 0;
}

/* 不要硬编码 ACCEPT：应用的定制框可能用 GTK_RESPONSE_OK 或自定义正数 id。
 * 依次找一个真实存在的「接受」按钮，把它的 response id 原样返回。 */
static int accept_response(gptr dialog)
{
    fn_get_widget_for_response widget_for = (fn_get_widget_for_response)sym("gtk_dialog_get_widget_for_response");
    if (widget_for) {
        static int const candidates[] = { RESP_ACCEPT, RESP_OK, RESP_YES, RESP_APPLY, 1, 2, 3, 4, 5 };
        for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            if (widget_for(dialog, candidates[i]))
                return candidates[i];
        }
    }
    fn_get_default_widget default_widget = (fn_get_default_widget)sym("gtk_window_get_default_widget");
    fn_get_response_for_widget response_for = (fn_get_response_for_widget)sym("gtk_dialog_get_response_for_widget");
    if (default_widget && response_for) {
        gptr widget = default_widget(dialog);
        if (widget) {
            int response = response_for(dialog, widget);
            if (response != RESP_NONE && response != RESP_CANCEL && response != RESP_DELETE_EVENT)
                return response;
        }
    }
    return RESP_ACCEPT;
}

/* helper 是独立 GUI 进程，需要桌面环境。父进程（GTK 应用）通常已经带了，
 * 这里补齐缺项，并去掉 LD_PRELOAD —— 免得 helper 又加载一次本 .so。 */
static char** helper_environment(void)
{
    int count = 0;
    while (environ[count])
        ++count;
    char** result = (char**)calloc(count + 10, sizeof(char*));
    int used = 0;
    for (int i = 0; i < count; ++i) {
        if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0)
            continue;
        result[used++] = environ[i];
    }
#define ENSURE(key, value)                            \
    do {                                              \
        if (!getenv(key))                             \
            result[used++] = (char*)(key "=" value);   \
    } while (0)
    ENSURE("SERENITY_RESOURCE_ROOT", "/res");
    ENSURE("SERENITY_BIN", "/serenity/bin");
    ENSURE("SERENITY_PREFIX", "/serenity");
    ENSURE("DISPLAY", ":100");
    ENSURE("WAYLAND_DISPLAY", "wayland-serenity-0");
#undef ENSURE
    static char library_path[4096];
    char const* existing = getenv("LD_LIBRARY_PATH");
    if (!existing || !strstr(existing, "/serenity/lib")) {
        snprintf(library_path, sizeof(library_path), "LD_LIBRARY_PATH=/serenity/lib%s%s",
            (existing && *existing) ? ":" : "", existing ? existing : "");
        result[used++] = library_path;
    }
    result[used] = 0;
    return result;
}

/* 同步启动 helper 并读它 stdout 的第一行（选中路径）。
 * 阻塞期间 GTK 应用自己的窗口不刷新，但原生选择器是另一个进程/WS 客户端，
 * 完全不受影响。返回 helper 退出码：0 选中、1 取消、其它失败。 */
static int run_helper(char** argv, char* out, size_t capacity)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    char** environment = helper_environment();
    pid_t pid = -1;
    int rc = posix_spawn(&pid, argv[0], &actions, 0, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    free(environment);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        log_line("spawn failed: %s", strerror(rc));
        return -1;
    }

    size_t used = 0;
    ssize_t got;
    while (used + 1 < capacity && (got = read(fds[0], out + used, capacity - used - 1)) > 0)
        used += (size_t)got;
    close(fds[0]);
    out[used] = 0;
    char* newline = strpbrk(out, "\r\n");
    if (newline)
        *newline = 0;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128;
}

/* 核心：弹原生选择器，把结果写回 chooser，返回该 dialog 的 response id。
 * 同步式由 gtk_dialog_run 直接 return；异步式由 idle 回调发出信号。 */
static int do_pick(gptr dialog, gptr chooser)
{
    fn_get_action action_of = (fn_get_action)sym("gtk_file_chooser_get_action");
    fn_get_cstring title_of = (fn_get_cstring)sym("gtk_window_get_title");
    fn_get_string folder_of = (fn_get_string)sym("gtk_file_chooser_get_current_folder");
    fn_get_string name_of = (fn_get_string)sym("gtk_file_chooser_get_current_name");
    fn_free gfree = (fn_free)sym("g_free");

    int action = action_of ? action_of(chooser) : FC_OPEN;
    int save = (action == FC_SAVE);
    int folder = (action == FC_SELECT_FOLDER || action == FC_CREATE_FOLDER);

    char const* picker = getenv("SERENITY_FILE_PICKER");
    if (!picker || !*picker)
        picker = "/serenity/bin/serenity-file-picker";

    char* argv[24] = { 0 };
    int argc = 0;
    argv[argc++] = (char*)picker;
    argv[argc++] = save ? "save" : "open";
    char const* title = title_of ? title_of(dialog) : 0;
    if (title && *title) {
        argv[argc++] = (char*)"--title";
        argv[argc++] = (char*)title;
    }
    if (folder)
        argv[argc++] = (char*)"--folder";
    char* start = folder_of ? folder_of(chooser) : 0;
    if (start && *start) {
        argv[argc++] = (char*)"--start";
        argv[argc++] = start;
    }
    char* name = 0;
    if (save) {
        name = name_of ? name_of(chooser) : 0;
        if (name && *name) {
            argv[argc++] = (char*)"--name";
            argv[argc++] = name;
        }
    }
    argv[argc] = 0;

    log_line("pick dialog=%s chooser=%s action=%d mode=%s title=%s start=%s name=%s",
        type_name(dialog), type_name(chooser), action, argv[1],
        title ? title : "-", start ? start : "-", name ? name : "-");

    char out[4096] = { 0 };
    int code = run_helper(argv, out, sizeof(out));
    log_line("helper exit=%d path=%s", code, out);

    if (gfree) {
        if (start)
            gfree(start);
        if (name)
            gfree(name);
    }

    int response = RESP_CANCEL;
    if (code == 0 && out[0]) {
        fn_set_string set_filename = (fn_set_string)sym("gtk_file_chooser_set_filename");
        if (set_filename)
            set_filename(chooser, out);
        /* SAVE 模式下 set_filename() 对不存在的路径会拆成 folder + name；
         * 再显式设一次 current_name，保证 get_filename() 能拼出目标路径。 */
        if (save) {
            char const* slash = strrchr(out, '/');
            char const* basename = slash ? slash + 1 : out;
            fn_set_string set_name = (fn_set_string)sym("gtk_file_chooser_set_current_name");
            if (set_name && *basename)
                set_name(chooser, basename);
        }
        response = accept_response(dialog);
    }
    log_line("result response=%d", response);
    return response;
}

/* ---- 入口 1：同步式（gtk_dialog_run） ---- */
int gtk_dialog_run(gptr dialog)
{
    fn_real_run real = (fn_real_run)dlsym(RTLD_NEXT, "gtk_dialog_run");
    if (!real)
        return RESP_NONE;
    if (!enabled() || !dialog || !implements(dialog, "gtk_dialog_get_type"))
        return real(dialog);
    gptr chooser = find_chooser(dialog, 0);
    if (!chooser)
        return real(dialog);
    log_line("run intercepted dialog=%s", type_name(dialog));
    return do_pick(dialog, chooser);
}

/* ---- 入口 2：异步式（gtk_window_present + "response" 信号） ----
 * gedit 48.1 实测只走这一条。不能在 present 里直接 gtk_dialog_response()：
 * 那时 gedit 还在自己的 action 调用栈里，且 dialog 尚未进入主循环；用
 * g_idle_add 延后到主循环转起来，再同步等 helper、写回、发 response。 */
static int on_pick_idle(gptr data)
{
    gptr dialog = data;
    gptr chooser = find_chooser(dialog, 0);
    int response = chooser ? do_pick(dialog, chooser) : RESP_CANCEL;

    fn_object_unref unref = (fn_object_unref)sym("g_object_unref");
    fn_dialog_response respond = (fn_dialog_response)sym("gtk_dialog_response");
    if (unref)
        unref(dialog);
    if (respond)
        respond(dialog, response);
    return 0; /* G_SOURCE_REMOVE */
}

void gtk_window_present(gptr window)
{
    fn_widget_void real = (fn_widget_void)dlsym(RTLD_NEXT, "gtk_window_present");
    if (!enabled() || !window || !implements(window, "gtk_dialog_get_type")) {
        if (real)
            real(window);
        return;
    }
    gptr chooser = find_chooser(window, 0);
    if (!chooser || already_handled(window)) {
        if (real)
            real(window);
        return;
    }
    /* 不调用真实 present —— GTK 那个对话框根本不显示，屏幕上只有原生选择器。 */
    log_line("present intercepted dialog=%s chooser=%s", type_name(window), type_name(chooser));
    fn_object_ref ref = (fn_object_ref)sym("g_object_ref");
    fn_idle_add idle_add = (fn_idle_add)sym("g_idle_add");
    if (!ref || !idle_add) {
        if (real)
            real(window);
        return;
    }
    idle_add(on_pick_idle, ref(window));
}

__attribute__((constructor)) static void on_load(void)
{
    if (enabled())
        log_line("loaded pid=%d", (int)getpid());
}
