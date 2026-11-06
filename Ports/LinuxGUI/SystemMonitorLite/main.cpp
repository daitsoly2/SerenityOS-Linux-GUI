/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Task manager for the Linux port (classic taskmgr layout).
//
// Layout follows the classic taskmgr: menu bar, tab strip, a process
// list with 映像名称 / 用户名 / CPU / 内存 / 磁盘 / 描述, a footer with the totals,
// and a performance tab with CPU + memory history graphs.
//
// Upstream SystemMonitor is not buildable here (LibSymbolication), so all data
// comes from /proc, the same sources /mnt/user/taskmgr uses without eBPF:
//   - CPU ticks / memory:   /proc/stat, /proc/meminfo
//   - per process:          /proc/<pid>/stat, status, exe, io
// /proc files report st_size = 0 and Core::File::read_until_eof() uses that as a
// heuristic, hence read_proc_file().

#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Model.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/TableView.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/UITranslation.h>
#include <LibGUI/Window.h>
#include <LibGfx/TextAlignment.h>
#include <LibMain/Main.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <pwd.h>
#include <string.h>
#include <signal.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

namespace {

// GUI::Model::invalidate() defaults to InvalidateAllIndices, which makes
// AbstractView::model_did_update() call clear_selection(). This app refreshes
// every second (500 ms on High), so that wiped the row the user just clicked
// before they could press "End Process" / "End Task". DontInvalidateIndices
// keeps the selection and the cursor/hover state; DontResizeColumns avoids
// re-measuring every cell on each tick and leaves manually sized columns alone.
static constexpr auto refresh_update_flags = static_cast<unsigned>(GUI::Model::UpdateFlag::DontInvalidateIndices)
    | static_cast<unsigned>(GUI::Model::UpdateFlag::DontResizeColumns);

struct ProcessInfo {
    int pid { 0 };
    int ppid { 0 };
    int threads { 1 };
    int nice { 0 };
    ByteString name;
    ByteString user;
    ByteString description;
    ByteString state;
    ByteString command_line;
    ByteString exe_path;
    u64 cpu_ticks { 0 };
    u64 rss_bytes { 0 };
    u64 io_bytes { 0 };
    double cpu_percent { 0 };
    double io_percent { 0 };
};

struct SystemStats {
    double cpu_percent { 0 };
    u64 memory_total { 0 };
    u64 memory_available { 0 };
    size_t process_count { 0 };
    size_t thread_count { 0 };
};

ErrorOr<ByteBuffer> read_proc_file(ByteString const& path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    ByteBuffer buffer;
    auto chunk = TRY(ByteBuffer::create_zeroed(8192));
    for (;;) {
        auto nread = file->read_some(chunk.bytes());
        if (nread.is_error())
            return nread.release_error();
        auto bytes_read = nread.value();
        if (bytes_read.is_empty())
            break;
        TRY(buffer.try_append(bytes_read.data(), bytes_read.size()));
        if (buffer.size() > 1 * MiB)
            break;
    }
    return buffer;
}

ByteString user_name_for_uid(uid_t uid)
{
    static HashMap<uid_t, ByteString> cache;
    if (auto it = cache.find(uid); it != cache.end())
        return it->value;

    ByteString name = ByteString::number(uid);
    if (auto* passwd = getpwuid(uid))
        name = passwd->pw_name;
    cache.set(uid, name);
    return name;
}

// Owner of a process, read from /proc/<pid>/status. Shared by the process table
// and the Applications tab so both show the same user name. The Uid: line is
// whitespace/tab separated ("Uid:\t1000\t1000\t1000\t1000"), so take the leading
// run of digits instead of splitting on a single space.
ByteString user_name_for_pid(int pid)
{
    if (auto status = read_proc_file(ByteString::formatted("/proc/{}/status", pid)); !status.is_error()) {
        for (auto line : StringView { status.value() }.split_view('\n')) {
            if (!line.starts_with("Uid:"sv))
                continue;
            auto values = line.substring_view(4).trim_whitespace();
            size_t end = 0;
            while (end < values.length() && values[end] >= '0' && values[end] <= '9')
                ++end;
            auto uid = AK::StringUtils::convert_to_uint<u64>(values.substring_view(0, end)).value_or(0);
            return user_name_for_uid(static_cast<uid_t>(uid));
        }
    }
    return "root"sv;
}

// Win7 "Processes"/"Details" grid: sortable columns, a name/PID filter and the
// "Show processes from all users" toggle. The extra columns (threads, status,
// command line) are hidden by default via column widths, but available to the
// View > Select Columns equivalent on the Details page.
class ProcessModel final : public GUI::Model {
public:
    enum Column {
        Name,
        Pid,
        User,
        Cpu,
        Memory,
        Disk,
        Threads,
        Status,
        Description,
        CommandLine,
        ColumnCount,
    };

    static NonnullRefPtr<ProcessModel> create() { return adopt_ref(*new ProcessModel); }

    void set_processes(Vector<ProcessInfo> processes)
    {
        m_all = move(processes);
        rebuild();
    }

    void set_filter(ByteString filter)
    {
        m_filter = move(filter);
        rebuild();
    }

    void set_show_all_users(bool show)
    {
        m_show_all_users = show;
        rebuild();
    }

    // Optional process tree (PPID hierarchy) with expand/collapse.
    void set_tree_mode(bool enabled)
    {
        m_tree_mode = enabled;
        rebuild();
    }

    void toggle_expanded(int row)
    {
        if (!m_tree_mode || row < 0 || row >= static_cast<int>(m_view.size()))
            return;
        if (!m_view[row].has_children)
            return;
        auto pid = m_view[row].process.pid;
        if (m_collapsed.find(pid) != m_collapsed.end())
            m_collapsed.remove(pid);
        else
            m_collapsed.set(pid, true);
        rebuild();
    }

    int pid_at(int row) const
    {
        if (auto const* process = process_at(row))
            return process->pid;
        return -1;
    }

    ProcessInfo const* process_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(m_view.size()))
            return nullptr;
        return &m_view[row].process;
    }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_view.size(); }
    virtual int column_count(GUI::ModelIndex const& = {}) const override { return ColumnCount; }

    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case Name:
            return GUI::translate_ui("Image Name"_string);
        case Pid:
            return "PID"_string;
        case User:
            return GUI::translate_ui("User Name"_string);
        case Cpu:
            return "CPU"_string;
        case Memory:
            return GUI::translate_ui("Memory"_string);
        case Disk:
            return GUI::translate_ui("Disk"_string);
        case Threads:
            return GUI::translate_ui("Threads"_string);
        case Status:
            return GUI::translate_ui("Status"_string);
        case Description:
            return GUI::translate_ui("Description"_string);
        case CommandLine:
            return GUI::translate_ui("Command Line"_string);
        default:
            return String {};
        }
    }

    virtual bool is_column_sortable(int) const override { return true; }

    virtual void sort(int column, GUI::SortOrder order) override
    {
        m_sort_column = column;
        m_sort_order = order;
        rebuild();
    }

    static ByteString state_text(ByteString const& state)
    {
        if (state.is_empty())
            return "未知"sv;
        switch (state[0]) {
        case 'R': return "正在运行"sv;
        case 'S': return "睡眠"sv;
        case 'D': return "等待"sv;
        case 'Z': return "僵尸"sv;
        case 'T': return "已停止"sv;
        default: return state;
        }
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role != GUI::ModelRole::Display || index.row() < 0 || index.row() >= static_cast<int>(m_view.size()))
            return {};
        auto const& view_row = m_view[index.row()];
        auto const& process = view_row.process;
        switch (index.column()) {
        case Name: {
            if (!m_tree_mode || view_row.depth == 0)
                return process.name;
            StringBuilder builder;
            for (int i = 0; i < view_row.depth; ++i)
                builder.append("  "sv);
            builder.append(view_row.has_children ? (view_row.expanded ? "[-] "sv : "[+] "sv) : "    "sv);
            builder.append(process.name);
            return builder.to_byte_string();
        }
        case Pid:
            return process.pid;
        case User:
            return process.user;
        case Cpu:
            return ByteString::formatted("{}", static_cast<int>(process.cpu_percent));
        case Memory:
            return ByteString::formatted("{} K", process.rss_bytes / 1024);
        case Disk:
            return ByteString::formatted("{} K/s", static_cast<u64>(process.io_percent) / 1024);
        case Threads:
            return process.threads;
        case Status:
            return state_text(process.state);
        case Description:
            return process.description;
        case CommandLine:
            return process.command_line;
        default:
            return {};
        }
    }

private:
    struct ViewRow {
        ProcessInfo process;
        int depth { 0 };
        bool has_children { false };
        bool expanded { true };
    };

    void rebuild()
    {
        // GUI::ModelIndex only stores a row number. Every rebuild can reorder
        // rows (the default CPU sort does so on each sample, and the user can
        // sort/filter/toggle the tree by hand), so remember the *PID* of each
        // selected row and look it up again once m_view has been rebuilt.
        // Otherwise the highlight would drift onto an unrelated process.
        Vector<int> selected_pids;
        Vector<int> selected_columns;
        for_each_view([&](GUI::AbstractView& view) {
            for (auto const& index : view.selection().indices()) {
                if (index.row() < 0 || index.row() >= static_cast<int>(m_view.size()))
                    continue;
                selected_pids.append(m_view[index.row()].process.pid);
                selected_columns.append(index.column());
            }
        });

        auto notify_and_restore_selection = [&] {
            did_update(refresh_update_flags);
            if (selected_pids.is_empty())
                return;
            for_each_view([&](GUI::AbstractView& view) {
                Vector<GUI::ModelIndex> restored;
                for (size_t i = 0; i < selected_pids.size(); ++i) {
                    for (int row = 0; row < static_cast<int>(m_view.size()); ++row) {
                        if (m_view[row].process.pid != selected_pids[i])
                            continue;
                        auto column = selected_columns[i];
                        if (column < 0 || column >= column_count())
                            column = 0;
                        restored.append(create_index(row, column));
                        break;
                    }
                }
                // Always clear first: the old row number may still be in range
                // while now pointing at a completely different process. If
                // none of the selected PIDs survived, leave nothing selected.
                view.selection().clear();
                if (!restored.is_empty())
                    view.selection().add_all(restored);
            });
        };

        auto filter = m_filter.to_lowercase();
        Vector<ProcessInfo> filtered;
        for (auto const& process : m_all) {
            if (!m_show_all_users && process.user != m_current_user)
                continue;
            if (!filter.is_empty()) {
                auto haystack = process.name.to_lowercase();
                if (!haystack.contains(filter) && !ByteString::number(process.pid).contains(filter)
                    && !process.description.to_lowercase().contains(filter))
                    continue;
            }
            filtered.append(process);
        }

        bool descending = m_sort_order != GUI::SortOrder::Ascending;
        quick_sort(filtered, [column = m_sort_column, descending](auto const& a, auto const& b) {
            auto compare = [&](auto const& left, auto const& right) {
                return descending ? left > right : left < right;
            };
            switch (column) {
            case Name: return compare(a.name, b.name);
            case Pid: return compare(a.pid, b.pid);
            case User: return compare(a.user, b.user);
            case Cpu: return compare(a.cpu_percent, b.cpu_percent);
            case Memory: return compare(a.rss_bytes, b.rss_bytes);
            case Disk: return compare(a.io_percent, b.io_percent);
            case Threads: return compare(a.threads, b.threads);
            case Status: return compare(a.state, b.state);
            case Description: return compare(a.description, b.description);
            case CommandLine: return compare(a.command_line, b.command_line);
            default: return compare(a.cpu_percent, b.cpu_percent);
            }
        });

        m_view.clear();
        if (!m_tree_mode) {
            for (auto& process : filtered)
                m_view.append({ move(process), 0, false, true });
            notify_and_restore_selection();
            return;
        }

        // Build the PPID hierarchy over the filtered set, then emit a
        // depth-first view with indentation and expand/collapse markers.
        HashMap<int, Vector<int>> children;
        HashMap<int, bool> present;
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i)
            present.set(filtered[i].pid, true);
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i)
            children.ensure(filtered[i].ppid).append(i);

        Vector<int> roots;
        for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
            if (filtered[i].pid == 1 || present.find(filtered[i].ppid) == present.end())
                roots.append(i);
        }

        Function<void(int, int)> emit = [&](int index, int depth) {
            auto const& process = filtered[index];
            auto kids = children.find(process.pid);
            bool has_children = kids != children.end() && !kids->value.is_empty();
            bool expanded = has_children && m_collapsed.find(process.pid) == m_collapsed.end();
            m_view.append({ process, depth, has_children, expanded });
            if (!expanded || kids == children.end())
                return;
            for (int child : kids->value)
                emit(child, depth + 1);
        };
        for (int root : roots)
            emit(root, 0);
        notify_and_restore_selection();
    }

    Vector<ProcessInfo> m_all;
    Vector<ViewRow> m_view;
    ByteString m_filter;
    ByteString m_current_user { user_name_for_uid(getuid()) };
    bool m_show_all_users { true };
    bool m_tree_mode { false };
    HashMap<int, bool> m_collapsed;
    int m_sort_column { Cpu };
    GUI::SortOrder m_sort_order { GUI::SortOrder::Descending };
};

long ticks_per_second()
{
    static long value = sysconf(_SC_CLK_TCK);
    return value > 0 ? value : 100;
}

Optional<ProcessInfo> read_process(int pid)
{
    auto contents = read_proc_file(ByteString::formatted("/proc/{}/stat", pid));
    if (contents.is_error())
        return {};

    StringView text { contents.value() };
    auto open_paren = text.find('(');
    auto close_paren = text.find_last(')');
    if (!open_paren.has_value() || !close_paren.has_value() || *close_paren < *open_paren)
        return {};

    ProcessInfo info;
    info.pid = pid;
    info.name = ByteString { text.substring_view(*open_paren + 1, *close_paren - *open_paren - 1) };

    auto rest = text.substring_view(*close_paren + 2);
    Vector<StringView> fields;
    rest.for_each_split_view(' ', SplitBehavior::Nothing, [&](StringView field) {
        fields.append(field);
    });
    if (fields.size() < 22)
        return {};

    auto to_u64 = [&](size_t index) { return AK::StringUtils::convert_to_uint<u64>(fields[index]).value_or(0); };
    info.state = ByteString { fields[0].bytes() };
    info.ppid = static_cast<int>(to_u64(1));
    info.cpu_ticks = to_u64(11) + to_u64(12);
    info.nice = static_cast<int>(to_u64(16));
    info.threads = static_cast<int>(to_u64(17));
    info.rss_bytes = to_u64(21) * static_cast<u64>(getpagesize());

    // Reading /proc/<pid>/cmdline for *every* process on every refresh was a
    // large part of the startup delay. It is only needed by the Properties
    // dialog, so read it on demand there instead.
    info.command_line = info.name;

    // Owner, like taskmgr's AppResolver/ProcessCollector pair.
    info.user = user_name_for_pid(pid);

    // Disk I/O, the cheap /proc version of taskmgr's BPF collector.
    if (auto io = read_proc_file(ByteString::formatted("/proc/{}/io", pid)); !io.is_error()) {
        for (auto line : StringView { io.value() }.split_view('\n')) {
            if (line.starts_with("read_bytes:"sv))
                info.io_bytes += AK::StringUtils::convert_to_uint<u64>(line.substring_view(11).trim_whitespace()).value_or(0);
            else if (line.starts_with("write_bytes:"sv))
                info.io_bytes += AK::StringUtils::convert_to_uint<u64>(line.substring_view(12).trim_whitespace()).value_or(0);
        }
    }

    char exe_buffer[PATH_MAX];
    if (auto length = readlink(ByteString::formatted("/proc/{}/exe", pid).characters(), exe_buffer, sizeof(exe_buffer) - 1); length > 0) {
        exe_buffer[length] = '\0';
        info.description = exe_buffer;
    } else {
        info.description = info.name;
    }
    return info;
}

struct CpuTicks {
    u64 total { 0 };
    u64 idle { 0 };
};

Optional<CpuTicks> read_cpu_ticks()
{
    auto contents = read_proc_file("/proc/stat"sv);
    if (contents.is_error())
        return {};

    auto lines = StringView { contents.value() }.split_view('\n');
    if (lines.is_empty())
        return {};
    auto first_line = lines.at(0);
    if (!first_line.starts_with("cpu "sv))
        return {};

    CpuTicks ticks;
    int index = 0;
    first_line.for_each_split_view(' ', SplitBehavior::Nothing, [&](StringView field) {
        if (index++ == 0)
            return;
        auto value = AK::StringUtils::convert_to_uint<u64>(field).value_or(0);
        ticks.total += value;
        auto field_index = index - 1;
        if (field_index == 4 || field_index == 5)
            ticks.idle += value;
    });
    return ticks;
}

struct MemoryInfo {
    u64 total { 0 };
    u64 available { 0 };
};

MemoryInfo read_memory_info()
{
    MemoryInfo info;
    auto contents = read_proc_file("/proc/meminfo"sv);
    if (contents.is_error())
        return info;

    for (auto line : StringView { contents.value() }.split_view('\n')) {
        auto value_kb = [&](StringView key) -> u64 {
            if (!line.starts_with(key))
                return 0;
            auto rest = line.substring_view(key.length()).trim_whitespace();
            return AK::StringUtils::convert_to_uint<u64>(rest.split_view(' ').at(0)).value_or(0);
        };
        if (auto value = value_kb("MemTotal:"sv); value != 0)
            info.total = value * 1024;
        else if (auto value = value_kb("MemAvailable:"sv); value != 0)
            info.available = value * 1024;
    }
    return info;
}

// Win7 performance graph: black background, green trace scrolling right to left.
class HistoryGraph final : public GUI::Widget {
    C_OBJECT(HistoryGraph)

public:
    HistoryGraph() = default;

    void set_title(ByteString title)
    {
        m_title = move(title);
        update();
    }

    void add_sample(double percent)
    {
        m_samples.append(clamp(percent, 0.0, 100.0));
        while (m_samples.size() > max_samples)
            m_samples.take_first();
        update();
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        auto inner = rect();

        painter.fill_rect(inner, Color::from_rgb(0x000000));
        painter.draw_text(Gfx::IntRect { 4, 2, width() - 8, 12 }, m_title, Gfx::TextAlignment::CenterLeft, Color::from_rgb(0x00ff00));

        // 25/50/75% grid, like the Win7 graph.
        for (int percent = 25; percent < 100; percent += 25) {
            auto y = inner.height() - 2 - static_cast<int>((inner.height() - 16) * percent / 100.0);
            if (y > 14)
                painter.draw_line({ 0, y }, { inner.width(), y }, Color::from_rgb(0x004400));
        }

        if (m_samples.size() > 1) {
            auto step = static_cast<double>(inner.width()) / static_cast<double>(max_samples - 1);
            auto baseline = inner.height() - 2;
            auto usable_height = inner.height() - 16;

            Gfx::IntPoint previous;
            bool has_previous = false;
            size_t index = max_samples - m_samples.size();
            for (auto sample : m_samples) {
                auto x = static_cast<int>(index * step);
                auto y = static_cast<int>(baseline - sample / 100.0 * usable_height);
                auto point = Gfx::IntPoint { x, clamp(y, 14, baseline) };
                if (has_previous)
                    painter.draw_line(previous, point, Color::from_rgb(0x00ff00));
                previous = point;
                has_previous = true;
                ++index;
            }
        }
    }

private:
    static constexpr size_t max_samples = 60;
    Vector<double> m_samples;
    ByteString m_title;
};


// --- 应用程序 / 联网 / GPU 数据源 -------------------------------------------

struct GuiApp {
    int pid { 0 };
    ByteString name;
    ByteString user;
};

// 谁算"应用程序"：Windows 任务管理器的这一页列的是有可见窗口的程序。
// 这套移植没有窗口列表 IPC（WindowServer / wlroots 都不对外暴露客户端清单），
// 所以只能近似，分两类：
//   * 本 prefix 下的 serenity-* 程序：沿用原来的后台 helper 名单过滤；
//   * 用户手动运行的原生 Linux 程序（gedit / xterm / firefox ...）：旧代码只认
//     serenity-* 前缀，把它们全漏掉了。这里要求进程处于图形会话（environ 里有
//     DISPLAY 或 WAYLAND_DISPLAY）、确实持有 socket fd（连到 X11 / Wayland
//     display），再排除已知的会话基础设施守护进程。
// environ / fd 读不到（别人的进程、或进程清了 dumpable）就跳过，不做猜测。

// /proc/<pid>/environ is a sequence of NUL-terminated "KEY=VALUE" entries.
// A graphical-session process always carries DISPLAY (X11) or WAYLAND_DISPLAY.
static bool has_display_environment(int pid)
{
    auto contents = read_proc_file(ByteString::formatted("/proc/{}/environ", pid));
    if (contents.is_error())
        return false;
    StringView env { contents.value() };
    size_t start = 0;
    while (start < env.length()) {
        size_t end = start;
        while (end < env.length() && env[end] != 0)
            ++end;
        auto entry = env.substring_view(start, end - start);
        if (entry.starts_with("DISPLAY="sv) || entry.starts_with("WAYLAND_DISPLAY="sv))
            return true;
        start = end + 1;
    }
    return false;
}

// A real GUI client holds at least one Unix socket fd (its X11 / Wayland display
// connection). A CLI child that merely inherited DISPLAY from a GUI terminal has
// none. Only the handful of processes that already passed the environ filter get
// here, so this does not bring back the old "walk every fd of every process" cost.
static bool holds_socket_fd(int pid)
{
    auto fd_dir = ByteString::formatted("/proc/{}/fd", pid);
    bool found = false;
    (void)Core::Directory::for_each_entry(fd_dir.view(), Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        char target[128];
        auto length = readlink(ByteString::formatted("{}/{}", fd_dir, entry.name).characters(), target, sizeof(target) - 1);
        if (length > 0) {
            target[length] = 0;
            if (strncmp(target, "socket:", 7) == 0)
                found = true;
        }
        return IterationDecision::Continue;
    });
    return found;
}

Vector<GuiApp> collect_gui_apps()
{
    auto prefix = ByteString::formatted("{}/bin/serenity-", PortHostify::install_prefix());
    Vector<GuiApp> apps;

    // Serenity 侧后台 helper（窗口服务器、各类 -server / -bridge 等）。
    static StringView const serenity_background_markers[] = {
        "-server"sv, "-bridge"sv, "-host"sv, "-grapher"sv, "-graph"sv,
        "-applet"sv, "-history"sv, "-portal"sv, "-network"sv,
        "-audio"sv, "-config"sv, "-launch"sv, "-clipboard"sv,
        "-resource"sv, "-netprobe"sv, "serenitid"sv, "serenity-shell"sv,
    };
    // 原生 Linux 会话基础设施：这些进程同样带着 DISPLAY，但它们是 dbus /
    // Xwayland / 辅助功能总线这类后台服务，不是用户手动打开的程序。
    static StringView const session_service_markers[] = {
        "dbus-daemon"sv, "dbus-broker"sv, "Xwayland"sv, "Xorg"sv, "Xvfb"sv,
        "at-spi"sv, "pipewire"sv, "wireplumber"sv, "pulseaudio"sv,
        "gpg-agent"sv, "ssh-agent"sv, "gnome-keyring"sv, "polkit"sv,
        "systemd"sv, "xdg-desktop-portal"sv, "xdg-document-portal"sv,
        "ibus"sv, "fcitx"sv, "gsd-"sv, "dconf-service"sv, "gvfsd"sv,
        "tracker-miner"sv, "evolution-source-registry"sv,
        "gnome-session"sv, "gnome-shell"sv,
    };

    (void)Core::Directory::for_each_entry("/proc"sv, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        auto pid = AK::StringUtils::convert_to_int<int>(entry.name);
        if (!pid.has_value())
            return IterationDecision::Continue;

        char exe_buffer[PATH_MAX];
        auto exe_length = readlink(ByteString::formatted("/proc/{}/exe", *pid).characters(), exe_buffer, sizeof(exe_buffer) - 1);
        if (exe_length <= 0)
            return IterationDecision::Continue;
        // readlink() does NOT NUL-terminate. Without this, strlen() reads stack
        // garbage and names come out as "serenity-audio-bridgerver", which also
        // silently defeats every ends_with("-server") style filter below.
        exe_buffer[exe_length] = 0;
        ByteString exe_path { exe_buffer, static_cast<size_t>(exe_length) };

        auto name = ByteString { LexicalPath::basename(exe_path) };

        if (exe_path.starts_with(prefix)) {
            // Serenity 应用：保持旧行为，只剔除已知后台 helper。
            for (auto marker : serenity_background_markers) {
                if (name.contains(marker))
                    return IterationDecision::Continue;
            }
        } else {
            // 原生 Linux 程序：必须在图形会话里、且确实连着 display；再排除
            // 会话守护进程。只看 DISPLAY 会把后台服务一起列进来，只靠名字
            // 黑名单又会误伤用户手动运行的任意程序，所以两者结合。
            if (!has_display_environment(*pid) || !holds_socket_fd(*pid))
                return IterationDecision::Continue;
            for (auto marker : session_service_markers) {
                if (name.contains(marker))
                    return IterationDecision::Continue;
            }
        }

        apps.append({ *pid, name, user_name_for_pid(*pid) });
        return IterationDecision::Continue;
    });

    quick_sort(apps, [](auto const& a, auto const& b) { return a.name < b.name; });
    return apps;
}

struct NetTotals {
    u64 rx_bytes { 0 };
    u64 tx_bytes { 0 };
};

// /proc/net/dev: lo 跳过，其余网卡累加。
NetTotals read_net_totals()
{
    NetTotals totals;
    auto contents = read_proc_file("/proc/net/dev"sv);
    if (contents.is_error())
        return totals;

    for (auto line : StringView { contents.value() }.split_view('\n')) {
        auto colon = line.find(':');
        if (!colon.has_value())
            continue;
        auto name = line.substring_view(0, *colon).trim_whitespace();
        if (name == "lo"sv || name == "Inter-|"sv || name.is_empty())
            continue;
        auto values = line.substring_view(*colon + 1).trim_whitespace().split_view(' ');
        Vector<u64> numbers;
        for (auto value : values) {
            if (value.is_empty())
                continue;
            numbers.append(AK::StringUtils::convert_to_uint<u64>(value).value_or(0));
        }
        if (numbers.size() < 9)
            continue;
        totals.rx_bytes += numbers[0];
        totals.tx_bytes += numbers[8];
    }
    return totals;
}

struct GpuStats {
    ByteString name;
    bool has_utilization { false };
    double utilization { 0 };
    u64 memory_used { 0 };
    u64 memory_total { 0 };
};

ByteString read_sysfs_line(ByteString const& path)
{
    auto contents = read_proc_file(path);
    if (contents.is_error())
        return {};
    return StringView { contents.value() }.trim_whitespace();
}

// 有 /sys 就读 /sys（amdgpu 有 gpu_busy_percent，Intel 只有频率），
// 什么都没有就只报适配器名，不假装有数据。
GpuStats read_gpu_stats()
{
    GpuStats stats;
    Core::DirIterator it("/sys/class/drm"sv, Core::DirIterator::SkipDots);
    while (!it.has_error() && it.has_next()) {
        auto entry = it.next_path();
        if (!entry.starts_with("card"sv) || entry.contains('-'))
            continue;
        auto base = ByteString::formatted("/sys/class/drm/{}/device", entry);

        auto driver = read_sysfs_line(ByteString::formatted("{}/uevent", base));
        for (auto line : driver.split_view('\n')) {
            if (line.starts_with("DRIVER="sv))
                stats.name = line.substring_view(7);
        }
        if (stats.name.is_empty())
            stats.name = entry;

        if (auto busy = read_sysfs_line(ByteString::formatted("{}/gpu_busy_percent", base)); !busy.is_empty()) {
            if (auto value = busy.to_number<u64>(); value.has_value()) {
                stats.utilization = static_cast<double>(value.value());
                stats.has_utilization = true;
            }
        }
        if (auto used = read_sysfs_line(ByteString::formatted("{}/mem_info_vram_used", base)); !used.is_empty())
            stats.memory_used = used.to_number<u64>().value_or(0);
        if (auto total = read_sysfs_line(ByteString::formatted("{}/mem_info_vram_total", base)); !total.is_empty())
            stats.memory_total = total.to_number<u64>().value_or(0);
        break;
    }
    return stats;
}

// --- 每进程网络（读 eBPF 采集器的输出） -------------------------------------

struct ProcessNet {
    int pid { 0 };
    ByteString name;
    u64 rx_total { 0 };
    u64 tx_total { 0 };
    double rx_rate { 0 };
    double tx_rate { 0 };
};

static ByteString format_byte_rate(double bytes_per_second)
{
    if (bytes_per_second < 1024.0)
        return ByteString::formatted("{:.0f} B/s", bytes_per_second);
    if (bytes_per_second < 1024.0 * 1024.0)
        return ByteString::formatted("{:.1f} K/s", bytes_per_second / 1024.0);
    return ByteString::formatted("{:.2f} M/s", bytes_per_second / (1024.0 * 1024.0));
}

static ByteString format_byte_total(u64 bytes)
{
    if (bytes < 1024ULL)
        return ByteString::formatted("{} B", bytes);
    if (bytes < 1024ULL * 1024ULL)
        return ByteString::formatted("{:.1f} K", static_cast<double>(bytes) / 1024.0);
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
        return ByteString::formatted("{:.1f} M", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return ByteString::formatted("{:.2f} G", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

static ByteString process_name_for_pid(int pid)
{
    if (auto comm = read_proc_file(ByteString::formatted("/proc/{}/comm", pid)); !comm.is_error()) {
        auto name = StringView { comm.value() }.trim_whitespace();
        if (!name.is_empty())
            return name;
    }
    char buffer[PATH_MAX];
    if (readlink(ByteString::formatted("/proc/{}/exe", pid).characters(), buffer, sizeof(buffer) - 1) > 0)
        return LexicalPath::basename(buffer);
    return ByteString::formatted("pid {}", pid);
}

// 采集器输出格式：每行 "pid rx_bytes tx_bytes"。速率靠两次采样的差值算，
// 所以调用者要传真实经过的秒数。
Vector<ProcessNet> collect_process_network(double elapsed)
{
    static HashMap<int, NetTotals> previous;
    Vector<ProcessNet> samples;

    auto contents = read_proc_file("/tmp/serenity-netprobe.txt"sv);
    if (contents.is_error()) {
        previous.clear();
        return samples;
    }

    HashMap<int, bool> alive;
    for (auto line : StringView { contents.value() }.split_view('\n')) {
        auto fields = line.trim_whitespace().split_view(' ');
        if (fields.size() < 3)
            continue;
        auto pid = fields[0].to_number<int>();
        auto rx = fields[1].to_number<u64>();
        auto tx = fields[2].to_number<u64>();
        if (!pid.has_value() || !rx.has_value() || !tx.has_value())
            continue;

        ProcessNet sample;
        sample.pid = pid.value();
        sample.name = process_name_for_pid(sample.pid);
        sample.rx_total = rx.value();
        sample.tx_total = tx.value();
        if (auto it = previous.find(sample.pid); it != previous.end() && elapsed > 0) {
            sample.rx_rate = sample.rx_total >= it->value.rx_bytes
                ? static_cast<double>(sample.rx_total - it->value.rx_bytes) / elapsed
                : 0.0;
            sample.tx_rate = sample.tx_total >= it->value.tx_bytes
                ? static_cast<double>(sample.tx_total - it->value.tx_bytes) / elapsed
                : 0.0;
        }
        previous.set(sample.pid, NetTotals { sample.rx_total, sample.tx_total });
        alive.set(sample.pid, true);
        samples.append(move(sample));
    }

    // 已经退出的进程从历史里剔掉，否则表越来越大。
    Vector<int> dead;
    for (auto const& entry : previous) {
        if (!alive.contains(entry.key))
            dead.append(entry.key);
    }
    for (auto pid : dead)
        previous.remove(pid);

    quick_sort(samples, [](auto const& a, auto const& b) {
        return (a.rx_rate + a.tx_rate) > (b.rx_rate + b.tx_rate);
    });
    return samples;
}

// 名字要单独查 /proc，所以先收完再补名字（避免排序前对每个 pid 都读两次文件）。
class NetModel final : public GUI::Model {
public:
    enum Column {
        Process,
        Pid,
        RxRate,
        TxRate,
        RxTotal,
        TxTotal,
        ColumnCount,
    };

    static NonnullRefPtr<NetModel> create() { return adopt_ref(*new NetModel); }

    void set_samples(Vector<ProcessNet> samples)
    {
        m_samples = move(samples);
        did_update(refresh_update_flags);
    }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_samples.size(); }
    virtual int column_count(GUI::ModelIndex const& = {}) const override { return ColumnCount; }
    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case Process:
            return GUI::translate_ui("Process"_string);
        case Pid:
            return GUI::translate_ui("PID"_string);
        case RxRate:
            return GUI::translate_ui("Received/s"_string);
        case TxRate:
            return GUI::translate_ui("Sent/s"_string);
        case RxTotal:
            return GUI::translate_ui("Received total"_string);
        case TxTotal:
            return GUI::translate_ui("Sent total"_string);
        default:
            return String {};
        }
    }
    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role != GUI::ModelRole::Display || index.row() < 0 || index.row() >= static_cast<int>(m_samples.size()))
            return {};
        auto const& sample = m_samples[index.row()];
        switch (index.column()) {
        case Process:
            return sample.name;
        case Pid:
            return sample.pid;
        case RxRate:
            return format_byte_rate(sample.rx_rate);
        case TxRate:
            return format_byte_rate(sample.tx_rate);
        case RxTotal:
            return format_byte_total(sample.rx_total);
        case TxTotal:
            return format_byte_total(sample.tx_total);
        default:
            return {};
        }
    }

private:
    Vector<ProcessNet> m_samples;
};

class AppModel final : public GUI::Model {
public:
    enum Column {
        Task,
        Status,
        Pid,
        User,
        ColumnCount,
    };

    static NonnullRefPtr<AppModel> create() { return adopt_ref(*new AppModel); }

    void set_apps(Vector<GuiApp> apps)
    {
        m_apps = move(apps);
        did_update(refresh_update_flags);
    }

    int pid_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(m_apps.size()))
            return -1;
        return m_apps[row].pid;
    }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_apps.size(); }
    virtual int column_count(GUI::ModelIndex const& = {}) const override { return ColumnCount; }
    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case Task:
            return GUI::translate_ui("Task"_string);
        case Status:
            return GUI::translate_ui("Status"_string);
        case Pid:
            return "PID"_string;
        case User:
            return GUI::translate_ui("User Name"_string);
        default:
            return String {};
        }
    }
    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role != GUI::ModelRole::Display || index.row() < 0 || index.row() >= static_cast<int>(m_apps.size()))
            return {};
        auto const& app = m_apps[index.row()];
        switch (index.column()) {
        case Task:
            return app.name;
        case Status:
            return GUI::translate_ui("Running"_string);
        case Pid:
            return app.pid;
        case User:
            return app.user;
        default:
            return {};
        }
    }

private:
    Vector<GuiApp> m_apps;
};

// --- Win7 "Services" tab -----------------------------------------------------
struct ServiceInfo {
    ByteString name;
    int pid { 0 };
    ByteString description;
    ByteString status;
    ByteString group;
};

class ServiceModel final : public GUI::Model {
public:
    enum Column { Name, Pid, Description, Status, Group, ColumnCount };

    static NonnullRefPtr<ServiceModel> create() { return adopt_ref(*new ServiceModel); }
    void set_services(Vector<ServiceInfo> services) { m_services = move(services); did_update(refresh_update_flags); }
    int pid_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(m_services.size()))
            return -1;
        return m_services[row].pid;
    }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_services.size(); }
    virtual int column_count(GUI::ModelIndex const& = {}) const override { return ColumnCount; }
    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case Name: return GUI::translate_ui("Name"_string);
        case Pid: return "PID"_string;
        case Description: return GUI::translate_ui("Description"_string);
        case Status: return GUI::translate_ui("Status"_string);
        case Group: return GUI::translate_ui("Group"_string);
        default: return String {};
        }
    }
    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role != GUI::ModelRole::Display || index.row() < 0 || index.row() >= static_cast<int>(m_services.size()))
            return {};
        auto const& service = m_services[index.row()];
        switch (index.column()) {
        case Name: return service.name;
        case Pid: return service.pid;
        case Description: return service.description;
        case Status: return service.status;
        case Group: return service.group;
        default: return {};
        }
    }

private:
    Vector<ServiceInfo> m_services;
};

// --- Win7 "Users" tab --------------------------------------------------------
struct UserInfo {
    ByteString name;
    ByteString id;
    ByteString status;
    ByteString client;
    ByteString session;
    int process_count { 0 };
};

class UserModel final : public GUI::Model {
public:
    enum Column { User, Id, Status, Client, Session, ColumnCount };

    static NonnullRefPtr<UserModel> create() { return adopt_ref(*new UserModel); }
    void set_users(Vector<UserInfo> users) { m_users = move(users); did_update(refresh_update_flags); }

    virtual int row_count(GUI::ModelIndex const& = {}) const override { return m_users.size(); }
    virtual int column_count(GUI::ModelIndex const& = {}) const override { return ColumnCount; }
    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case User: return GUI::translate_ui("User"_string);
        case Id: return "ID"_string;
        case Status: return GUI::translate_ui("Status"_string);
        case Client: return GUI::translate_ui("Client Name"_string);
        case Session: return GUI::translate_ui("Session"_string);
        default: return String {};
        }
    }
    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (role != GUI::ModelRole::Display || index.row() < 0 || index.row() >= static_cast<int>(m_users.size()))
            return {};
        auto const& user = m_users[index.row()];
        switch (index.column()) {
        case User: return user.name;
        case Id: return user.id;
        case Status: return user.status;
        case Client: return user.client;
        case Session: return user.session;
        default: return {};
        }
    }

private:
    Vector<UserInfo> m_users;
};

// Win7-style service list: the port has no service manager IPC, so identify
// well-known daemons from /proc and report them as running services.
Vector<ServiceInfo> collect_services(Vector<ProcessInfo> const& processes)
{
    static char const* const service_names[] = {
        "dbus-daemon", "fcitx5", "Xwayland", "pipewire", "pipewire-pulse", "wireplumber",
        "systemd", "systemd-logind", "polkitd", "NetworkManager", "wpa_supplicant",
        "serenity-window-server", "serenity-wlroots-host", "serenity-portal",
        "serenity-audio-server", "serenity-clipboard-server", "serenity-config-server",
        "serenity-launch-server", "serenity-shell",
    };
    Vector<ServiceInfo> services;
    for (auto const* name : service_names) {
        for (auto const& process : processes) {
            if (process.name != name)
                continue;
            services.append({ name, process.pid, process.description,
                GUI::translate_ui("Running"_string).to_byte_string(), "Serenity"sv });
            break;
        }
    }
    quick_sort(services, [](auto const& a, auto const& b) { return a.name < b.name; });
    return services;
}

Vector<UserInfo> collect_users(Vector<ProcessInfo> const& processes)
{
    HashMap<ByteString, UserInfo> by_user;
    for (auto const& process : processes) {
        auto it = by_user.find(process.user);
        if (it == by_user.end()) {
            by_user.set(process.user, { process.user, "-"sv, "活动"sv, "本地"sv, "控制台"sv, 0 });
            it = by_user.find(process.user);
        }
        it->value.process_count++;
    }
    Vector<UserInfo> users;
    for (auto const& entry : by_user)
        users.append(entry.value);
    quick_sort(users, [](auto const& a, auto const& b) { return a.name < b.name; });
    return users;
}

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    int lock_fd = open("/tmp/.serenity-task-manager.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
        return 0;

    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title(GUI::translate_ui("Task Manager"_string).to_byte_string());
    window->resize(660, 460);

    auto widget = GUI::Widget::construct();
    window->set_main_widget(widget);
    widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 2, 2, 2, 2 }, 2);
    // Root widget must paint its background or transparent Label areas show black.
    widget->set_fill_with_background_color(true);

    auto& tabs = widget->add<GUI::TabWidget>();

    // =====================================================================
    // 应用程序 (Applications) -- Win7: Task | Status, End/Switch/New Task
    // =====================================================================
    auto& apps_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Applications"_string));
    apps_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 4, 4, 4, 4 }, 4);
    apps_page.set_fill_with_background_color(true);

    auto app_model = AppModel::create();
    auto& apps_table = apps_page.add<GUI::TableView>();
    apps_table.set_model(app_model);
    apps_table.set_alternating_row_colors(true);
    apps_table.set_column_width(AppModel::Task, 400);
    apps_table.set_column_width(AppModel::Status, 110);
    apps_table.set_column_visible(AppModel::Pid, false);
    apps_table.set_column_visible(AppModel::User, false);

    auto& apps_footer = apps_page.add<GUI::Widget>();
    apps_footer.set_fixed_height(30);
    apps_footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    auto& apps_end_button = apps_footer.add<GUI::Button>(GUI::translate_ui("End Task"_string));
    apps_end_button.set_fixed_width(110);
    auto& apps_switch_button = apps_footer.add<GUI::Button>(GUI::translate_ui("Switch To"_string));
    apps_switch_button.set_fixed_width(100);
    auto& apps_new_button = apps_footer.add<GUI::Button>(GUI::translate_ui("New Task..."_string));
    apps_new_button.set_fixed_width(110);
    auto& apps_spacer = apps_footer.add<GUI::Widget>();
    apps_spacer.set_fill_with_background_color(true);
    auto& apps_label = apps_footer.add<GUI::Label>("..."_string);
    apps_label.set_fixed_width(100);

    // 文件 > 新建任务(运行…)：Win7 的 Run 对话框。
    auto show_new_task = [&] {
        auto dialog = GUI::Dialog::construct(window.ptr());
        dialog->set_title(GUI::translate_ui("Create New Task"_string).to_byte_string());
        dialog->resize(440, 140);

        auto content = dialog->set_main_widget<GUI::Widget>();
        content->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8, 8, 8, 8 }, 6);
        content->set_fill_with_background_color(true);

        auto& prompt = content->add<GUI::Label>(GUI::translate_ui("Type the name of a program, folder, document or Internet resource"_string));
        prompt.set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& input = content->add<GUI::TextBox>();
        input.set_fixed_height(24);

        auto& buttons = content->add<GUI::Widget>();
        buttons.set_fixed_height(28);
        buttons.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
        buttons.add<GUI::Widget>(); // left spacer
        auto& run_button = buttons.add<GUI::Button>(GUI::translate_ui("OK"_string));
        run_button.set_fixed_width(90);
        auto& cancel_button = buttons.add<GUI::Button>(GUI::translate_ui("Cancel"_string));
        cancel_button.set_fixed_width(90);

        run_button.on_click = [dialog, &input](unsigned) {
            auto command = input.text();
            if (!command.is_empty()) {
                Vector<ByteString> argv { "-c"sv, command };
                if (auto result = Core::Process::spawn("/bin/sh"sv, argv); result.is_error())
                    warnln("task manager: cannot run {}: {}", command, result.error());
            }
            dialog->close();
        };
        cancel_button.on_click = [dialog](unsigned) { dialog->close(); };
        input.on_return_pressed = [&] { run_button.click(); };
        dialog->show();
    };

    // =====================================================================
    // 进程 (Processes) -- Win7: filter, "show all users", sortable columns
    // =====================================================================
    auto& process_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Processes"_string));
    process_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 4, 4, 4, 4 }, 4);
    process_page.set_fill_with_background_color(true);

    auto process_model = ProcessModel::create();
    auto& process_table = process_page.add<GUI::TableView>();
    process_table.set_model(process_model);
    process_table.set_alternating_row_colors(true);
    process_table.set_column_width(ProcessModel::Name, 170);
    process_table.set_column_width(ProcessModel::Pid, 55);
    process_table.set_column_width(ProcessModel::User, 75);
    process_table.set_column_width(ProcessModel::Cpu, 45);
    process_table.set_column_width(ProcessModel::Memory, 80);
    process_table.set_column_width(ProcessModel::Disk, 80);
    process_table.set_column_width(ProcessModel::Threads, 55);
    process_table.set_column_width(ProcessModel::Status, 75);
    process_table.set_column_width(ProcessModel::Description, 170);
    process_table.set_column_width(ProcessModel::CommandLine, 200);
    // Win7 default columns: name / user / CPU / memory / description.
    process_table.set_column_visible(ProcessModel::Pid, false);
    process_table.set_column_visible(ProcessModel::Threads, false);
    process_table.set_column_visible(ProcessModel::Status, false);
    process_table.set_column_visible(ProcessModel::CommandLine, false);
    process_table.set_column_visible(ProcessModel::Disk, false);

    auto& process_footer = process_page.add<GUI::Widget>();
    process_footer.set_fixed_height(30);
    process_footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    auto& kill_button = process_footer.add<GUI::Button>(GUI::translate_ui("End Process"_string));
    kill_button.set_fixed_width(110);
    auto& show_all_checkbox = process_footer.add<GUI::CheckBox>(GUI::translate_ui("Show processes from all users"_string));
    show_all_checkbox.set_checked(true);
    show_all_checkbox.set_fixed_width(180);
    auto& tree_checkbox = process_footer.add<GUI::CheckBox>(GUI::translate_ui("Show process tree"_string));
    tree_checkbox.set_fixed_width(110);
    auto& process_spacer = process_footer.add<GUI::Widget>();
    process_spacer.set_fill_with_background_color(true);
    auto& footer_label = process_footer.add<GUI::Label>("..."_string);
    footer_label.set_fixed_width(160);

    // =====================================================================
    // 服务 (Services)
    // =====================================================================
    auto& services_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Services"_string));
    services_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 4, 4, 4, 4 }, 4);
    services_page.set_fill_with_background_color(true);

    auto service_model = ServiceModel::create();
    auto& services_table = services_page.add<GUI::TableView>();
    services_table.set_model(service_model);
    services_table.set_alternating_row_colors(true);
    services_table.set_column_width(ServiceModel::Name, 150);
    services_table.set_column_width(ServiceModel::Pid, 55);
    services_table.set_column_width(ServiceModel::Description, 220);
    services_table.set_column_width(ServiceModel::Status, 80);
    services_table.set_column_width(ServiceModel::Group, 80);

    auto& services_footer = services_page.add<GUI::Widget>();
    services_footer.set_fixed_height(30);
    services_footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    auto& services_button = services_footer.add<GUI::Button>(GUI::translate_ui("Services..."_string));
    services_button.set_fixed_width(110);
    auto& services_spacer = services_footer.add<GUI::Widget>();
    services_spacer.set_fill_with_background_color(true);
    auto& services_label = services_footer.add<GUI::Label>(GUI::translate_ui("Right-click a service to open its process"_string));
    services_label.set_fixed_width(220);

    // =====================================================================
    // 性能 (Performance) -- Win7: two graphs + system/memory/kernel stats
    // =====================================================================
    auto& performance_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Performance"_string));
    performance_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6, 6, 6, 6 }, 6);
    performance_page.set_fill_with_background_color(true);

    auto& cpu_header = performance_page.add<GUI::Label>(MUST(String::formatted("{}: 0%", GUI::translate_ui("CPU Usage"_string))));
    auto& cpu_graph = performance_page.add<HistoryGraph>();
    cpu_graph.set_title(GUI::translate_ui("CPU Usage"_string).to_byte_string());
    cpu_graph.set_fixed_height(52);

    performance_page.add<GUI::Label>(GUI::translate_ui("CPU Usage History"_string));
    auto& cpu_history_graph = performance_page.add<HistoryGraph>();
    cpu_history_graph.set_title(GUI::translate_ui("CPU Usage History"_string).to_byte_string());
    cpu_history_graph.set_fixed_height(52);

    auto& memory_header = performance_page.add<GUI::Label>(MUST(String::formatted("{}: 0%", GUI::translate_ui("Physical Memory Usage"_string))));
    auto& memory_graph = performance_page.add<HistoryGraph>();
    memory_graph.set_title(GUI::translate_ui("Memory"_string).to_byte_string());
    memory_graph.set_fixed_height(52);

    performance_page.add<GUI::Label>(GUI::translate_ui("Physical Memory Usage History"_string));
    auto& memory_history_graph = performance_page.add<HistoryGraph>();
    memory_history_graph.set_title(GUI::translate_ui("Physical Memory Usage History"_string).to_byte_string());
    memory_history_graph.set_fixed_height(52);

    auto& stats_system_label = performance_page.add<GUI::Label>("..."_string);
    stats_system_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    auto& stats_memory_label = performance_page.add<GUI::Label>("..."_string);
    stats_memory_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    auto& stats_totals_label = performance_page.add<GUI::Label>("..."_string);
    stats_totals_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    auto& performance_footer = performance_page.add<GUI::Widget>();
    performance_footer.set_fixed_height(30);
    performance_footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    auto& resource_monitor_button = performance_footer.add<GUI::Button>(GUI::translate_ui("Resource Monitor..."_string));
    resource_monitor_button.set_fixed_width(170);
    auto& performance_spacer = performance_footer.add<GUI::Widget>();
    performance_spacer.set_fill_with_background_color(true);

    // =====================================================================
    // 联网 (Networking)
    // =====================================================================
    auto& network_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Networking"_string));
    network_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6, 6, 6, 6 }, 6);
    network_page.set_fill_with_background_color(true);

    auto& network_header = network_page.add<GUI::Label>(MUST(String::formatted("{}: 0 K/s", GUI::translate_ui("Throughput"_string))));
    auto& rx_header = network_page.add<GUI::Label>(MUST(String::formatted("{}: 0 K/s", GUI::translate_ui("Received"_string))));
    auto& rx_graph = network_page.add<HistoryGraph>();
    rx_graph.set_title(GUI::translate_ui("Network Utilization"_string).to_byte_string());
    auto& tx_header = network_page.add<GUI::Label>(MUST(String::formatted("{}: 0 K/s", GUI::translate_ui("Sent"_string))));
    auto& tx_graph = network_page.add<HistoryGraph>();
    tx_graph.set_title(GUI::translate_ui("Network Utilization"_string).to_byte_string());
    rx_graph.set_fixed_height(60);
    tx_graph.set_fixed_height(60);

    auto net_model = NetModel::create();
    auto& net_table = network_page.add<GUI::TableView>();
    net_table.set_model(net_model);
    net_table.set_alternating_row_colors(true);
    net_table.set_column_width(NetModel::Process, 140);
    net_table.set_column_width(NetModel::Pid, 55);
    net_table.set_column_width(NetModel::RxRate, 80);
    net_table.set_column_width(NetModel::TxRate, 80);
    net_table.set_column_width(NetModel::RxTotal, 80);

    // =====================================================================
    // 用户 (Users)
    // =====================================================================
    auto& users_page = tabs.add_tab<GUI::Widget>(GUI::translate_ui("Users"_string));
    users_page.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 4, 4, 4, 4 }, 4);
    users_page.set_fill_with_background_color(true);

    auto user_model = UserModel::create();
    auto& users_table = users_page.add<GUI::TableView>();
    users_table.set_model(user_model);
    users_table.set_alternating_row_colors(true);
    users_table.set_column_width(UserModel::User, 130);
    users_table.set_column_width(UserModel::Id, 55);
    users_table.set_column_width(UserModel::Status, 80);
    users_table.set_column_width(UserModel::Client, 100);
    users_table.set_column_width(UserModel::Session, 80);

    auto& users_footer = users_page.add<GUI::Widget>();
    users_footer.set_fixed_height(30);
    users_footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 6);
    auto& logoff_button = users_footer.add<GUI::Button>(GUI::translate_ui("Log off"_string));
    logoff_button.set_fixed_width(100);
    logoff_button.set_enabled(false); // no logind session in this port
    auto& disconnect_button = users_footer.add<GUI::Button>(GUI::translate_ui("Disconnect"_string));
    disconnect_button.set_fixed_width(100);
    disconnect_button.set_enabled(false);
    auto& users_spacer = users_footer.add<GUI::Widget>();
    users_spacer.set_fill_with_background_color(true);
    auto& users_label = users_footer.add<GUI::Label>(GUI::translate_ui("Session management is handled by serenity-logout"_string));
    users_label.set_fixed_width(220);

    // =====================================================================
    // Global status bar (Win7 shows process count / CPU / memory here)
    // =====================================================================
    auto& statusbar = widget->add<GUI::Statusbar>(3);
    statusbar.set_text(0, "..."_string);
    statusbar.set_text(1, "..."_string);
    statusbar.set_text(2, "..."_string);

    // =====================================================================
    // Menu bar: 文件 / 选项 / 查看 / 窗口 / 帮助
    // =====================================================================
    bool refresh_now = false;

    auto file_menu = window->add_menu(MUST(String::from_utf8("&File"sv)));
    file_menu->add_action(GUI::Action::create(GUI::translate_ui("New Task (Run...)"_string).to_byte_string(), [&](auto&) {
        show_new_task();
    }));
    file_menu->add_separator();
    file_menu->add_action(GUI::Action::create(GUI::translate_ui("Exit Task Manager"_string).to_byte_string(), [&](auto&) {
        app->quit(0);
    }));

    auto options_menu = window->add_menu(MUST(String::from_utf8("&Options"sv)));
    auto always_on_top = GUI::Action::create_checkable(
        GUI::translate_ui("Always on Top"_string).to_byte_string(), [&](GUI::Action& action) {
            window->set_always_on_top(action.is_checked());
        });
    options_menu->add_action(always_on_top);
    auto minimize_on_use = GUI::Action::create_checkable(
        GUI::translate_ui("Minimize on use"_string).to_byte_string(), [&](GUI::Action& action) {
            if (action.is_checked())
                window->set_minimized(true);
        });
    options_menu->add_action(minimize_on_use);

    auto add_disabled = [](GUI::Menu& menu, String label) {
        auto action = GUI::Action::create(label.to_byte_string(), [](auto&) {});
        action->set_enabled(false);
        menu.add_action(action);
    };
    add_disabled(*options_menu, GUI::translate_ui("Hide When Minimized"_string));
    add_disabled(*options_menu, GUI::translate_ui("Show 16-bit Tasks"_string));

    auto view_menu = window->add_menu(MUST(String::from_utf8("&View"sv)));
    view_menu->add_action(GUI::Action::create(GUI::translate_ui("Refresh Now"_string).to_byte_string(), [&](auto&) {
        refresh_now = true;
    }));

    int refresh_interval_ms = 1000; // 标准 / Normal
    Vector<RefPtr<GUI::Action>> speed_actions;
    auto make_speed = [&](String label, int interval) {
        auto action = GUI::Action::create_checkable(label.to_byte_string(), [&, interval](GUI::Action& self) {
            refresh_interval_ms = interval;
            for (auto& other : speed_actions) {
                if (other.ptr() != &self)
                    other->set_checked(false);
            }
        });
        view_menu->add_action(action);
        speed_actions.append(action);
        if (interval == 1000)
            action->set_checked(true);
    };
    view_menu->add_separator();
    make_speed(GUI::translate_ui("Update Speed > High"_string), 500);
    make_speed(GUI::translate_ui("Update Speed > Normal"_string), 1000);
    make_speed(GUI::translate_ui("Update Speed > Low"_string), 4000);
    make_speed(GUI::translate_ui("Update Speed > Paused"_string), 0);

    view_menu->add_separator();
    auto add_column_toggle = [&](String label, int column, bool initially_visible) {
        auto action = GUI::Action::create_checkable(label.to_byte_string(), [&, column](GUI::Action& self) {
            process_table.set_column_visible(column, self.is_checked());
        });
        action->set_checked(initially_visible);
        view_menu->add_action(action);
    };
    add_column_toggle(GUI::translate_ui("Select Columns > PID"_string), ProcessModel::Pid, false);
    add_column_toggle(GUI::translate_ui("Select Columns > Threads"_string), ProcessModel::Threads, false);
    add_column_toggle(GUI::translate_ui("Select Columns > Status"_string), ProcessModel::Status, false);
    add_column_toggle(GUI::translate_ui("Select Columns > Command Line"_string), ProcessModel::CommandLine, false);
    view_menu->add_separator();
    add_disabled(*view_menu, GUI::translate_ui("Show Kernel Times"_string));

    auto window_menu = window->add_menu(MUST(String::from_utf8("&Window"sv)));
    add_disabled(*window_menu, GUI::translate_ui("Cascade Windows"_string));
    add_disabled(*window_menu, GUI::translate_ui("Tile Windows Horizontally"_string));
    add_disabled(*window_menu, GUI::translate_ui("Tile Windows Vertically"_string));
    window_menu->add_separator();
    window_menu->add_action(GUI::Action::create(GUI::translate_ui("Switch To"_string).to_byte_string(), [&](auto&) {
        tabs.set_tab_index(0);
    }));
    window_menu->add_action(GUI::Action::create(GUI::translate_ui("End Task"_string).to_byte_string(), [&](auto&) {
        apps_end_button.click();
    }));
    window_menu->add_separator();
    add_disabled(*window_menu, GUI::translate_ui("Bring To Front"_string));
    add_disabled(*window_menu, GUI::translate_ui("Minimize"_string));
    add_disabled(*window_menu, GUI::translate_ui("Maximize"_string));
    add_disabled(*window_menu, GUI::translate_ui("Close"_string));

    auto help_menu = window->add_menu(MUST(String::from_utf8("帮助"sv)));
    add_disabled(*help_menu, GUI::translate_ui("Task Manager Help Topics"_string));
    help_menu->add_separator();
    help_menu->add_action(GUI::Action::create(GUI::translate_ui("About Task Manager"_string).to_byte_string(), [&](auto&) {
        GUI::MessageBox::show(window.ptr(),
            MUST(String::formatted("{} (Linux port)\n\n{}/proc", GUI::translate_ui("Task Manager"_string), GUI::translate_ui("Data source"_string))),
            GUI::translate_ui("About"_string), GUI::MessageBox::Type::Information);
    }));

    // =====================================================================
    // Sampling
    // =====================================================================
    HashMap<int, u64> previous_ticks;
    HashMap<int, u64> previous_io;
    auto previous_time = time(nullptr);
    Optional<CpuTicks> previous_cpu_ticks;
    Vector<ProcessInfo> last_processes;

    auto refresh = [&] {
        // GUI::ModelIndex identifies a cell by row number, and the default CPU
        // sort reshuffles rows on every sample. A row-based selection would
        // silently point at a *different* process -- dangerous when the next
        // click is "End Process". ProcessModel::rebuild() already re-anchors
        // its own selection by PID; the remaining models have no other rebuild
        // trigger, so snapshot their selection by stable identity (PID, or the
        // user name on the Users tab) and restore it after this refresh.
        auto selected_pid = [](GUI::TableView const& view, int pid_column) -> int {
            auto const* model = view.model();
            auto index = view.selection().first();
            if (!model || !index.is_valid())
                return -1;
            return model->index(index.row(), pid_column).data().to_i32();
        };
        auto selected_name = [](GUI::TableView const& view, int name_column) -> ByteString {
            auto const* model = view.model();
            auto index = view.selection().first();
            if (!model || !index.is_valid())
                return {};
            return model->index(index.row(), name_column).data().to_byte_string();
        };
        auto selected_column = [](GUI::TableView const& view) -> int {
            auto index = view.selection().first();
            return index.is_valid() ? index.column() : 0;
        };
        auto restore_by_pid = [](GUI::TableView& view, int pid_column, int pid, int column) {
            if (pid < 0)
                return;
            // Drop the old row first -- it may still be in range but now hold
            // a different item. If the PID vanished, the selection stays empty.
            view.selection().clear();
            auto* model = view.model();
            if (!model)
                return;
            for (int row = 0; row < model->row_count(); ++row) {
                if (model->index(row, pid_column).data().to_i32() != pid)
                    continue;
                view.selection().set(model->index(row, clamp(column, 0, model->column_count() - 1)));
                return;
            }
        };
        auto restore_by_name = [](GUI::TableView& view, int name_column, ByteString const& name, int column) {
            if (name.is_empty())
                return;
            view.selection().clear();
            auto* model = view.model();
            if (!model)
                return;
            for (int row = 0; row < model->row_count(); ++row) {
                if (model->index(row, name_column).data().to_byte_string() != name)
                    continue;
                view.selection().set(model->index(row, clamp(column, 0, model->column_count() - 1)));
                return;
            }
        };

        int saved_app_pid = selected_pid(apps_table, AppModel::Pid);
        int saved_app_column = selected_column(apps_table);
        int saved_service_pid = selected_pid(services_table, ServiceModel::Pid);
        int saved_service_column = selected_column(services_table);
        int saved_net_pid = selected_pid(net_table, NetModel::Pid);
        int saved_net_column = selected_column(net_table);
        auto saved_user_name = selected_name(users_table, UserModel::User);
        int saved_user_column = selected_column(users_table);

        struct timespec sample_start {};
        clock_gettime(CLOCK_MONOTONIC, &sample_start);
        auto now = time(nullptr);
        auto elapsed = max<long>(1, now - previous_time);
        previous_time = now;

        SystemStats stats;
        if (auto cpu_ticks = read_cpu_ticks(); cpu_ticks.has_value()) {
            if (previous_cpu_ticks.has_value()) {
                auto total_delta = cpu_ticks->total > previous_cpu_ticks->total ? cpu_ticks->total - previous_cpu_ticks->total : 0;
                auto idle_delta = cpu_ticks->idle > previous_cpu_ticks->idle ? cpu_ticks->idle - previous_cpu_ticks->idle : 0;
                if (total_delta > 0)
                    stats.cpu_percent = (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta)) * 100.0;
            }
            previous_cpu_ticks = cpu_ticks;
        }

        auto memory = read_memory_info();
        stats.memory_total = memory.total;
        stats.memory_available = memory.available;
        auto memory_used = stats.memory_total > stats.memory_available ? stats.memory_total - stats.memory_available : 0;
        auto memory_percent = stats.memory_total > 0 ? (memory_used * 100.0 / stats.memory_total) : 0.0;

        Vector<ProcessInfo> processes;
        // Win7 collapses kernel work into a single "System" row. Linux exposes
        // dozens of kthreads (ksoftirqd, rcu_*, migration/*), all children of
        // kthreadd (pid 2). Aggregate them into one "内核进程" entry instead of
        // flooding the process list.
        ProcessInfo kernel_process;
        bool has_kernel_process = false;
        (void)Core::Directory::for_each_entry("/proc"sv, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
            auto pid = AK::StringUtils::convert_to_int<int>(entry.name);
            if (!pid.has_value())
                return IterationDecision::Continue;
            if (auto process = read_process(*pid); process.has_value()) {
                auto previous = previous_ticks.get(process->pid).value_or(process->cpu_ticks);
                auto delta = process->cpu_ticks >= previous ? process->cpu_ticks - previous : 0;
                process->cpu_percent = (static_cast<double>(delta) / ticks_per_second()) / static_cast<double>(elapsed) * 100.0;
                previous_ticks.set(process->pid, process->cpu_ticks);

                auto previous_io_bytes = previous_io.get(process->pid).value_or(process->io_bytes);
                process->io_percent = process->io_bytes >= previous_io_bytes ? static_cast<double>(process->io_bytes - previous_io_bytes) / elapsed : 0.0;
                previous_io.set(process->pid, process->io_bytes);

                stats.thread_count += max(1, process->threads);

                if (process->ppid == 2 || process->pid == 2) {
                    if (!has_kernel_process) {
                        kernel_process.pid = 2;
                        kernel_process.name = "内核进程"sv;
                        kernel_process.user = "SYSTEM"sv;
                        kernel_process.state = "S"sv;
                        kernel_process.description = "Linux 内核"sv;
                        kernel_process.command_line = "[kernel threads]"sv;
                        has_kernel_process = true;
                    }
                    kernel_process.cpu_percent += process->cpu_percent;
                    kernel_process.rss_bytes += process->rss_bytes;
                    kernel_process.io_percent += process->io_percent;
                    kernel_process.threads += process->threads;
                    return IterationDecision::Continue;
                }
                processes.append(*process);
            }
            return IterationDecision::Continue;
        });
        if (has_kernel_process)
            processes.append(kernel_process);
        stats.process_count = processes.size();
        last_processes = processes;
        process_model->set_processes(processes);
        service_model->set_services(collect_services(last_processes));
        user_model->set_users(collect_users(last_processes));

        footer_label.set_text(MUST(String::formatted("{}: {}    CPU: {:.0f}%    {}: {:.0f}%",
            GUI::translate_ui("Processes"_string), stats.process_count,
            stats.cpu_percent, GUI::translate_ui("Memory"_string), memory_percent)));

        statusbar.set_text(0, MUST(String::formatted("{}: {}", GUI::translate_ui("Processes"_string), stats.process_count)));
        statusbar.set_text(1, MUST(String::formatted("CPU: {:.0f}%", stats.cpu_percent)));
        statusbar.set_text(2, MUST(String::formatted("{}: {:.0f}%", GUI::translate_ui("Memory"_string), memory_percent)));

        cpu_header.set_text(MUST(String::formatted("{}: {:.0f}%", GUI::translate_ui("CPU Usage"_string), stats.cpu_percent)));
        memory_header.set_text(MUST(String::formatted("{}: {:.0f}%  ({} / {})",
            GUI::translate_ui("Physical Memory Usage"_string), memory_percent,
            ByteString::formatted("{:.0f} MiB", memory_used / (1024.0 * 1024.0)),
            ByteString::formatted("{:.0f} MiB", stats.memory_total / (1024.0 * 1024.0)))));
        cpu_graph.add_sample(stats.cpu_percent);
        cpu_history_graph.add_sample(stats.cpu_percent);
        memory_graph.add_sample(memory_percent);
        memory_history_graph.add_sample(memory_percent);

        stats_system_label.set_text(MUST(String::formatted("{}    {}: {}    {}: {}    {}: {}",
            GUI::translate_ui("System"_string), GUI::translate_ui("Processes"_string), stats.process_count,
            GUI::translate_ui("Threads"_string), stats.thread_count,
            GUI::translate_ui("Total"_string), stats.process_count + stats.thread_count)));
        stats_memory_label.set_text(MUST(String::formatted("{}    {}: {:.0f} MiB    {}: {:.0f} MiB    {}: {:.0f} MiB",
            GUI::translate_ui("Physical Memory"_string),
            GUI::translate_ui("Total"_string), stats.memory_total / (1024.0 * 1024.0),
            GUI::translate_ui("Available"_string), stats.memory_available / (1024.0 * 1024.0),
            GUI::translate_ui("In use"_string), memory_used / (1024.0 * 1024.0))));
        stats_totals_label.set_text(MUST(String::formatted("{}: {}", GUI::translate_ui("Adapter"_string), read_gpu_stats().name)));

        auto apps = collect_gui_apps();
        apps_label.set_text(MUST(String::formatted("{}: {}", GUI::translate_ui("Applications"_string), apps.size())));
        app_model->set_apps(move(apps));

        auto net = read_net_totals();
        static u64 previous_rx = 0;
        static u64 previous_tx = 0;
        auto rx_rate = net.rx_bytes >= previous_rx ? static_cast<double>(net.rx_bytes - previous_rx) / elapsed : 0.0;
        auto tx_rate = net.tx_bytes >= previous_tx ? static_cast<double>(net.tx_bytes - previous_tx) / elapsed : 0.0;
        previous_rx = net.rx_bytes;
        previous_tx = net.tx_bytes;
        constexpr double network_full_scale = 1024.0 * 1024.0;
        rx_header.set_text(MUST(String::formatted("{}: {:.1f} K/s", GUI::translate_ui("Received"_string), rx_rate / 1024.0)));
        tx_header.set_text(MUST(String::formatted("{}: {:.1f} K/s", GUI::translate_ui("Sent"_string), tx_rate / 1024.0)));
        network_header.set_text(MUST(String::formatted("{}: {:.1f} K/s", GUI::translate_ui("Throughput"_string), (rx_rate + tx_rate) / 1024.0)));
        rx_graph.add_sample(rx_rate / network_full_scale * 100.0);
        tx_graph.add_sample(tx_rate / network_full_scale * 100.0);
        net_model->set_samples(collect_process_network(static_cast<double>(elapsed)));

        // Restore the selection by identity, not by row number. If the item
        // disappeared (or was just killed) no row matches, so the selection
        // stays empty -- which is exactly what we want.
        restore_by_pid(apps_table, AppModel::Pid, saved_app_pid, saved_app_column);
        restore_by_pid(services_table, ServiceModel::Pid, saved_service_pid, saved_service_column);
        restore_by_pid(net_table, NetModel::Pid, saved_net_pid, saved_net_column);
        restore_by_name(users_table, UserModel::User, saved_user_name, saved_user_column);

        static bool first_sample = true;
        if (first_sample) {
            struct timespec sample_end {};
            clock_gettime(CLOCK_MONOTONIC, &sample_end);
            auto milliseconds = (sample_end.tv_sec - sample_start.tv_sec) * 1000
                + (sample_end.tv_nsec - sample_start.tv_nsec) / 1000000;
            warnln("task manager: first sample {} ms ({} processes)", milliseconds, stats.process_count);
            first_sample = false;
        }
    };

    // =====================================================================
    // Actions: end process / end process tree / priority / properties
    // =====================================================================
    auto selected_process = [&](GUI::TableView const& view, ProcessModel const& model) -> ProcessInfo const* {
        auto selection = view.selection().first();
        if (!selection.is_valid())
            return nullptr;
        return model.process_at(selection.row());
    };

    auto kill_pid = [&](int pid, int signal_number) {
        if (pid == 2) // aggregated kernel row: not a killable process
            return;
        if (pid > 1 && pid != getpid())
            (void)kill(pid, signal_number);
    };

    auto kill_tree = [&](int root_pid, int signal_number) {
        // Win7 "End Process Tree": children first, then the root.
        bool changed = true;
        for (int depth = 0; depth < 8 && changed; ++depth) {
            changed = false;
            for (auto const& process : last_processes) {
                if (process.ppid == root_pid && process.pid != getpid()) {
                    kill_pid(process.pid, signal_number);
                    root_pid = process.pid; // walk down one level next pass
                    changed = true;
                    break;
                }
            }
        }
        kill_pid(root_pid, signal_number);
    };

    auto show_properties = [&](ProcessInfo const& process) {
        auto command_line = process.command_line;
        if (auto cmdline = read_proc_file(ByteString::formatted("/proc/{}/cmdline", process.pid)); !cmdline.is_error()) {
            StringBuilder builder;
            for (char c : cmdline.value().bytes())
                builder.append(c == '\0' ? ' ' : c);
            command_line = builder.to_byte_string().trim_whitespace();
        }
        GUI::MessageBox::show(window.ptr(),
            MUST(String::formatted("{}: {}\n{}: {}\n{}: {}\n{}: {}\n{}: {}\n{}: {}\n{}: {}",
                GUI::translate_ui("Image Name"_string), process.name,
                GUI::translate_ui("PID"_string), process.pid,
                GUI::translate_ui("User Name"_string), process.user,
                GUI::translate_ui("Status"_string), ProcessModel::state_text(process.state),
                GUI::translate_ui("Threads"_string), process.threads,
                GUI::translate_ui("Path"_string), process.description,
                GUI::translate_ui("Command Line"_string), command_line)),
            GUI::translate_ui("Properties"_string), GUI::MessageBox::Type::Information);
    };

    auto set_priority = [&](ProcessInfo const& process, int nice_value) {
        if (setpriority(PRIO_PROCESS, process.pid, nice_value) < 0)
            warnln("task manager: setpriority({}) failed: {}", process.pid, strerror(errno));
    };

    auto process_context_menu = GUI::Menu::construct();
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("End Process"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            kill_pid(p->pid, SIGTERM);
        refresh();
    }));
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("End Process Tree"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            kill_tree(p->pid, SIGTERM);
        refresh();
    }));
    process_context_menu->add_separator();
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Set Priority > High"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            set_priority(*p, -10);
    }));
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Set Priority > Normal"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            set_priority(*p, 0);
    }));
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Set Priority > Low"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            set_priority(*p, 10);
    }));
    process_context_menu->add_separator();
    process_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Properties"_string).to_byte_string(), [&](auto&) {
        if (auto* p = selected_process(process_table, *process_model))
            show_properties(*p);
    }));
    process_table.on_context_menu_request = [&](GUI::ModelIndex const& index, GUI::ContextMenuEvent const& event) {
        if (index.is_valid())
            process_table.selection().set(index);
        process_context_menu->popup(event.screen_position());
    };

    auto apps_context_menu = GUI::Menu::construct();
    apps_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Switch To"_string).to_byte_string(), [&](auto&) {
        tabs.set_tab_index(0);
    }));
    apps_context_menu->add_action(GUI::Action::create(GUI::translate_ui("End Task"_string).to_byte_string(), [&](auto&) {
        apps_end_button.click();
    }));
    apps_context_menu->add_action(GUI::Action::create(GUI::translate_ui("Go to Process"_string).to_byte_string(), [&](auto&) {
        tabs.set_tab_index(1);
    }));
    apps_table.on_context_menu_request = [&](GUI::ModelIndex const& index, GUI::ContextMenuEvent const& event) {
        if (index.is_valid())
            apps_table.selection().set(index);
        apps_context_menu->popup(event.screen_position());
    };

    kill_button.on_click = [&](unsigned) {
        if (auto* p = selected_process(process_table, *process_model))
            kill_pid(p->pid, SIGTERM);
        refresh();
    };
    apps_end_button.on_click = [&](unsigned) {
        auto selection = apps_table.selection().first();
        if (!selection.is_valid())
            return;
        auto pid = app_model->pid_at(selection.row());
        kill_pid(pid, SIGTERM);
        refresh();
    };
    apps_switch_button.on_click = [&](unsigned) { tabs.set_tab_index(0); };
    apps_new_button.on_click = [&](unsigned) { show_new_task(); };
    services_button.on_click = [&](unsigned) { tabs.set_tab_index(2); };
    resource_monitor_button.on_click = [&](unsigned) { tabs.set_tab_index(3); };

    show_all_checkbox.on_checked = [&](bool checked) {
        process_model->set_show_all_users(checked);
    };
    tree_checkbox.on_checked = [&](bool checked) {
        process_model->set_tree_mode(checked);
    };
    process_table.on_activation = [&](GUI::ModelIndex const& index) {
        process_model->toggle_expanded(index.row());
    };

    window->show();
    // Paint the window first. The initial /proc sweep runs on the next event
    // loop turn, so startup no longer blocks on hundreds of file reads.
    auto first_refresh = Core::Timer::create_single_shot(0, [&] { refresh(); });
    first_refresh->start();
    int elapsed_since_refresh = 0;
    auto timer = Core::Timer::create_repeating(250, [&] {
        if (refresh_now) {
            refresh_now = false;
            elapsed_since_refresh = 0;
            refresh();
            return;
        }
        if (refresh_interval_ms == 0)
            return;
        elapsed_since_refresh += 250;
        if (elapsed_since_refresh >= refresh_interval_ms) {
            elapsed_since_refresh = 0;
            refresh();
        }
    });
    timer->start();

    return app->exec();
}
