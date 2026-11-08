/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// 打印管理 - a Win9x-style Control Panel applet for the Linux GUI port that
// talks to the host's CUPS print system over the standard CUPS CLI
// (lpstat / lpadmin / cupsenable / cupsdisable / cupsaccept / cupsreject /
// lp / cancel / lpinfo). No libcups dependency: every value shown here comes
// from the same tools an administrator uses, so what the panel shows is
// exactly what the rest of the system sees.
//
// Tabs:
//   打印机   - list of CUPS queues with live state (空闲 / 打印中 / 已停用),
//              plus 添加 / 删除 / 设为默认 / 启用 禁用 / 接受 拒绝任务 /
//              打印测试页.
//   打印任务 - jobs in flight (lpstat -W not-completed), with 取消 support.
//
// Privilege model: read-only commands work unprivileged. Queue administration
// is allowed for members of the lpadmin group (the port installs the session
// user into it). When that is not enough (e.g. the desktop session was started
// before the group change), calls are retried through `sudo -n`; nothing is
// sent anywhere and no password is stored.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCore/Command.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/Icon.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Model.h>
#include <LibGUI/ScrollableContainerWidget.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/TableView.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <unistd.h>

namespace {

// --- CUPS command plumbing --------------------------------------------------

ByteString resolve_tool(ByteString const& name)
{
    // Administration tools live in /usr/sbin; lpstat & friends in /usr/bin.
    // Absolute paths keep working no matter what PATH the session exports
    // (this desktop does not put /usr/sbin on PATH). On merged-usr systems
    // /usr/sbin is a symlink target shared with /usr/bin; fall back to a
    // plain PATH lookup otherwise.
    for (auto prefix : { "/usr/sbin/"sv, "/usr/bin/"sv, "/sbin/"sv, "/bin/"sv }) {
        auto candidate = ByteString::formatted("{}{}", prefix, name);
        if (access(candidate.characters(), X_OK) == 0)
            return candidate;
    }
    return name;
}

struct ToolResult {
    bool spawned { false }; // false when the tool itself could not be started
    int exit_code { -1 };
    ByteString output;
    ByteString error;
};

bool sudo_works()
{
    static Optional<bool> cached;
    if (cached.has_value())
        return *cached;
    auto result = Core::command("/usr/bin/sudo", { "-n"sv, "true"sv }, {});
    cached = result.is_error() ? false : result.value().exit_code == 0;
    return *cached;
}

ByteString buffer_text(ByteBuffer const& buffer)
{
    return String::from_utf8_with_replacement_character(StringView { buffer }).to_byte_string();
}

// Run `tool args...` with LC_ALL=C so output is parseable regardless of the
// user's locale. Queue administration commands are retried via `sudo -n`
// when the direct call fails and a non-interactive sudo is available;
// read-only calls (allow_sudo_fallback=false) never escalate.
ToolResult run_cups(ByteString const& tool, Vector<ByteString> const& arguments, bool allow_sudo_fallback)
{
    auto resolved = resolve_tool(tool);

    Vector<ByteString> env_args { ByteString { "LC_ALL=C"sv }, resolved };
    env_args.extend(arguments);
    auto direct = Core::command("/usr/bin/env", env_args, {});

    if (direct.is_error()) {
        // Fall back to a plain PATH lookup for tools in unusual locations.
        Vector<ByteString> plain_args { ByteString { "LC_ALL=C"sv }, tool };
        plain_args.extend(arguments);
        direct = Core::command("/usr/bin/env", plain_args, {});
    }

    ToolResult result;
    if (!direct.is_error()) {
        result.spawned = true;
        result.exit_code = direct.value().exit_code;
        result.output = buffer_text(direct.value().output);
        result.error = buffer_text(direct.value().error);
        if (result.exit_code == 0 || !allow_sudo_fallback || !sudo_works())
            return result;
    } else if (!allow_sudo_fallback) {
        return result;
    }

    if (!allow_sudo_fallback)
        return result;

    // Retry as root through non-interactive sudo.
    Vector<ByteString> sudo_args { ByteString { "LC_ALL=C"sv }, ByteString { "sudo"sv }, ByteString { "-n"sv }, resolved };
    sudo_args.extend(arguments);
    auto elevated = Core::command("/usr/bin/env", sudo_args, {});
    if (elevated.is_error())
        return result; // Keep the original failure: it carries the real message.
    ToolResult elevated_result;
    elevated_result.spawned = true;
    elevated_result.exit_code = elevated.value().exit_code;
    elevated_result.output = buffer_text(elevated.value().output);
    elevated_result.error = buffer_text(elevated.value().error);
    return elevated_result;
}

ByteString bytes_to_human(u64 bytes)
{
    if (bytes < 1024)
        return ByteString::formatted("{} B", bytes);
    if (bytes < 1024 * 1024)
        return ByteString::formatted("{} KB", bytes / 1024);
    return ByteString::formatted("{:.1f} MB", static_cast<double>(bytes) / 1024.0 / 1024.0);
}

// --- Data model --------------------------------------------------------------

struct Printer {
    ByteString name;
    ByteString description;
    ByteString location;
    ByteString device_uri;
    ByteString state_message; // e.g. "The printer may not exist ..." on backend failure
    bool printing { false };  // queue is actively processing a job
    bool disabled { false };  // scheduler stopped (cupsdisable)
    bool rejecting { false }; // queue is rejecting new jobs (cupsreject)
    bool is_default { false };

    ByteString status_text() const
    {
        ByteString status;
        if (disabled)
            status = "已停用";
        else if (printing)
            status = "打印中";
        else
            status = "空闲";
        if (rejecting)
            status = ByteString::formatted("{} · 拒绝任务", status);
        return status;
    }
};

struct PrintJob {
    ByteString id;      // e.g. "office-42"
    ByteString printer; // "office"
    ByteString user;
    u64 size_bytes { 0 };
    ByteString submitted; // raw text from lpstat, e.g. "Sat Nov  7 11:34:46 2026"
};

Vector<Printer> query_printers()
{
    Vector<Printer> printers;

    auto result = run_cups("lpstat", { "-l"sv, "-p"sv }, false);
    if (!result.spawned || result.exit_code != 0)
        return printers;

    Printer current;
    bool in_entry = false;
    auto flush = [&]() {
        if (in_entry)
            printers.append(move(current));
        current = {};
        in_entry = false;
    };

    static StringView const ignored_prefixes[] = {
        "Form mounted:"sv, "Content types:"sv, "Printer types:"sv, "Alerts:"sv,
        "Connection:"sv, "On fault:"sv, "After fault:"sv, "Users allowed:"sv,
        "Forms allowed:"sv, "Banner required"sv, "Charset sets:"sv, "Default "sv,
        "Contents of"sv,
    };

    for (auto raw : result.output.split_view('\n')) {
        auto line = raw.trim_whitespace();
        if (line.is_empty())
            continue;
        if (raw.starts_with("printer "sv)) {
            flush();
            in_entry = true;
            auto rest = line.substring_view("printer "sv.length());
            auto first_space = rest.find(' ');
            current.name = ByteString { first_space.has_value() ? rest.substring_view(0, first_space.value()) : rest };
            auto state = first_space.has_value() ? rest.substring_view(first_space.value() + 1) : StringView {};
            current.printing = state.contains("now printing"sv);
            current.disabled = state.contains("disabled since"sv);
            continue;
        }
        if (!in_entry)
            continue;
        // Indented detail lines. We track Description / Location / the
        // "Rejecting Jobs" flag; anything unrecognized that is worded like a
        // state message (e.g. backend failures) is surfaced verbatim.
        auto value_after = [&](StringView key) -> Optional<StringView> {
            if (line.starts_with(key))
                return line.substring_view(key.length()).trim_whitespace();
            return {};
        };
        if (auto value = value_after("Description:"sv); value.has_value()) {
            current.description = ByteString { *value };
            continue;
        }
        if (auto value = value_after("Location:"sv); value.has_value()) {
            current.location = ByteString { *value };
            continue;
        }
        if (line == "Rejecting Jobs"sv) {
            current.rejecting = true;
            continue;
        }
        bool known = false;
        for (auto const& prefix : ignored_prefixes) {
            if (line.starts_with(prefix)) {
                known = true;
                break;
            }
        }
        if (!known && line.contains("may not exist"sv))
            current.state_message = ByteString { line };
    }
    flush();

    // Device URIs (lpstat -v), keyed by printer name.
    auto devices = run_cups("lpstat", { "-v"sv }, false);
    if (devices.spawned && devices.exit_code == 0) {
        for (auto raw : devices.output.split_view('\n')) {
            auto line = raw.trim_whitespace();
            // "device for <name>: <uri>"
            if (!line.starts_with("device for "sv))
                continue;
            auto rest = line.substring_view("device for "sv.length());
            auto colon = rest.find(": "sv);
            if (!colon.has_value())
                continue;
            auto name = rest.substring_view(0, colon.value());
            auto uri = rest.substring_view(colon.value() + 2);
            for (auto& p : printers) {
                if (p.name == name)
                    p.device_uri = ByteString { uri };
            }
        }
    }

    // Default destination.
    auto def = run_cups("lpstat", { "-d"sv }, false);
    if (def.spawned && def.exit_code == 0) {
        for (auto raw : def.output.split_view('\n')) {
            auto line = raw.trim_whitespace();
            if (!line.contains("default destination"sv) && !line.contains("默认目的地"sv))
                continue;
            if (line.contains("no "sv) || line.contains("无"sv))
                continue;
            // "system default destination: <name>"
            auto colon = line.find(':');
            if (!colon.has_value())
                continue;
            auto name = line.substring_view(colon.value() + 1).trim_whitespace();
            for (auto& p : printers) {
                if (p.name == name)
                    p.is_default = true;
            }
        }
    }

    return printers;
}

// --- IPP Get-Printer-Attributes (via ipptool -X, textual plist read) ---------

// Live per-queue attributes that lpstat/lpoptions do not expose. printers.conf
// is stale for these (cupsd does not rewrite it on -o changes), so the truth
// comes from IPP itself. No libcups/libxml dependency: ipptool is part of the
// cups-ipp-utils package (installed with cups) and its -X output is a plist we
// read with a tiny dedicated parser for the scalar tags we need.
struct IppAttributes {
    bool valid { false }; // false when ipptool could not reach the scheduler/queue
    bool accepting { true };
    bool shared { true };
    bool enabled { true };
    ByteString error_policy { "retry-job"sv };
    int quota_period { 0 }; // seconds; 0 = no limit
    int page_limit { 0 };   // pages per user per quota period; 0 = no limit
    int k_limit { 0 };      // KB per user per quota period; 0 = no limit
    ByteString make_and_model;
    ByteString info;
    ByteString location;
    ByteString device_uri;
};

Optional<ByteString> plist_value(StringView xml, StringView key)
{
    auto needle = ByteString::formatted("<key>{}</key>", key);
    auto start = xml.find(needle);
    if (!start.has_value())
        return {};
    auto rest = xml.substring_view(start.value() + needle.length());
    auto lt = rest.find('<');
    if (!lt.has_value())
        return {};
    auto tag = rest.substring_view(lt.value());

    auto unescape = [](ByteString in) {
        in = in.replace("&amp;"sv, "&"sv);
        in = in.replace("&lt;"sv, "<"sv);
        in = in.replace("&gt;"sv, ">"sv);
        in = in.replace("&quot;"sv, "\""sv);
        in = in.replace("&apos;"sv, "'"sv);
        return in;
    };
    auto value_of = [&](StringView open, StringView close) -> Optional<ByteString> {
        if (!tag.starts_with(open))
            return {};
        auto end = tag.find(close);
        if (!end.has_value())
            return {};
        return unescape(ByteString { tag.substring_view(open.length(), end.value() - open.length()) });
    };
    if (auto value = value_of("<string>"sv, "</string>"sv); value.has_value())
        return value;
    if (auto value = value_of("<integer>"sv, "</integer>"sv); value.has_value())
        return value;
    if (tag.starts_with("<true"sv))
        return ByteString { "true"sv };
    if (tag.starts_with("<false"sv))
        return ByteString { "false"sv };
    return {};
}

IppAttributes query_ipp_attributes(ByteString const& name)
{
    IppAttributes attributes;

    auto uri = ByteString::formatted("ipp://localhost:631/printers/{}", name);
    auto test_path = ByteString::formatted("/tmp/serenity-ipp-attrs-{}.test", getpid());
    {
        auto test_content = ByteString::formatted(
            "{{ OPERATION get-printer-attributes\n"
            " NAME \"serenity-get-printer-attributes\"\n"
            " GROUP operation\n"
            " ATTR charset attributes-charset utf-8\n"
            " ATTR language attributes-natural-language en\n"
            " ATTR uri printer-uri {}\n"
            " ATTR keyword requested-attributes printer-is-shared,printer-is-accepting-jobs,printer-error-policy,job-quota-period,job-page-limit,job-k-limit,printer-make-and-model,printer-info,printer-location,printer-state,device-uri\n"
            " STATUS successful-ok\n"
            "}}\n",
            uri);
        FILE* fp = fopen(test_path.characters(), "w");
        if (!fp)
            return attributes;
        fwrite(test_content.characters(), 1, test_content.length(), fp);
        fclose(fp);
    }
    auto result = run_cups("ipptool", { "-X"sv, uri, test_path }, false);
    unlink(test_path.characters());
    if (!result.spawned || result.exit_code != 0)
        return attributes;

    auto& xml = result.output;
    auto string_of = [&](StringView key) { return plist_value(xml, key); };
    auto bool_of = [&](StringView key, bool fallback) {
        auto value = string_of(key);
        if (!value.has_value())
            return fallback;
        return *value == "true"sv;
    };
    auto int_of = [&](StringView key, int fallback) {
        auto value = string_of(key);
        if (!value.has_value())
            return fallback;
        return value->view().to_number<int>().value_or(fallback);
    };

    attributes.shared = bool_of("printer-is-shared"sv, true);
    attributes.accepting = bool_of("printer-is-accepting-jobs"sv, true);
    // printer-state: 3 = idle, 4 = processing, 5 = stopped
    attributes.enabled = int_of("printer-state"sv, 3) != 5;
    attributes.error_policy = string_of("printer-error-policy"sv).value_or("retry-job");
    attributes.quota_period = int_of("job-quota-period"sv, 0);
    attributes.page_limit = int_of("job-page-limit"sv, 0);
    attributes.k_limit = int_of("job-k-limit"sv, 0);
    attributes.make_and_model = string_of("printer-make-and-model"sv).value_or({});
    attributes.info = string_of("printer-info"sv).value_or({});
    attributes.location = string_of("printer-location"sv).value_or({});
    attributes.device_uri = string_of("device-uri"sv).value_or({});
    attributes.valid = true;
    return attributes;
}

// --- PPD options (lpoptions -l) ----------------------------------------------

struct QueueOption {
    ByteString key;          // e.g. "PageSize"
    ByteString label;        // e.g. "Media Size"
    Vector<ByteString> choices;
    int current { 0 };       // index into choices of the '*' (effective default) entry
};

Vector<QueueOption> query_queue_options(ByteString const& name)
{
    Vector<QueueOption> options;
    auto result = run_cups("lpoptions", { "-p"sv, name, "-l"sv }, false);
    if (!result.spawned || result.exit_code != 0)
        return options;
    for (auto raw : result.output.split_view('\n')) {
        auto line = raw.trim_whitespace();
        if (line.is_empty())
            continue;
        auto colon = line.find(':');
        if (!colon.has_value())
            continue;
        auto key_label = line.substring_view(0, colon.value()).trim_whitespace();
        QueueOption option;
        if (auto slash = key_label.find('/'); slash.has_value()) {
            option.key = ByteString { key_label.substring_view(0, slash.value()) };
            option.label = ByteString { key_label.substring_view(slash.value() + 1) };
        } else {
            option.key = ByteString { key_label };
            option.label = option.key;
        }
        int index = 0;
        int marked = -1;
        for (auto token : line.substring_view(colon.value() + 1).trim_whitespace().split_view(' ')) {
            if (token.is_empty())
                continue;
            bool is_default = token.starts_with('*');
            auto choice = is_default ? token.substring_view(1) : token;
            if (is_default)
                marked = index;
            option.choices.append(ByteString { choice });
            ++index;
        }
        if (option.choices.is_empty())
            continue;
        option.current = marked >= 0 ? marked : 0;
        options.append(move(option));
    }
    return options;
}

Vector<PrintJob> query_jobs()
{
    Vector<PrintJob> jobs;
    auto result = run_cups("lpstat", { "-W"sv, "not-completed"sv, "-o"sv }, false);
    if (!result.spawned || result.exit_code != 0)
        return jobs;

    for (auto raw : result.output.split_view('\n')) {
        auto line = raw.trim_whitespace();
        if (line.is_empty())
            continue;
        auto tokens = line.split_view(' ');
        if (tokens.size() < 3)
            continue;
        PrintJob job;
        job.id = ByteString { tokens[0] };
        if (auto dash = job.id.find_last('-'); dash.has_value())
            job.printer = job.id.substring(0, dash.value());
        else
            job.printer = job.id;
        job.user = ByteString { tokens[1] };
        job.size_bytes = tokens[2].to_number<u64>().value_or(0);
        if (tokens.size() > 3) {
            StringBuilder builder;
            for (size_t i = 3; i < tokens.size(); ++i) {
                if (i > 3)
                    builder.append(' ');
                builder.append(tokens[i]);
            }
            job.submitted = builder.to_byte_string();
        }
        jobs.append(move(job));
    }
    return jobs;
}

// --- Models ------------------------------------------------------------------

class PrinterModel final : public GUI::Model {
public:
    explicit PrinterModel(Vector<Printer> printers)
        : m_printers(move(printers))
    {
    }

    virtual int row_count(GUI::ModelIndex const&) const override { return m_printers.size(); }
    virtual int column_count(GUI::ModelIndex const& index) const override { return index.is_valid() ? 0 : 5; }

    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case 0:
            return "名称"_string;
        case 1:
            return "状态"_string;
        case 2:
            return "设备"_string;
        case 3:
            return "位置"_string;
        case 4:
            return "默认"_string;
        default:
            return "?"_string;
        }
    }

    virtual GUI::ModelIndex index(int row, int column, GUI::ModelIndex const&) const override
    {
        if (row < 0 || row >= (int)m_printers.size())
            return {};
        return create_index(row, column, &m_printers[row]);
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (index.row() < 0 || index.row() >= (int)m_printers.size())
            return {};
        auto& p = m_printers[index.row()];
        if (role == GUI::ModelRole::Search)
            return ByteString::formatted("{} {} {}", p.name, p.description, p.device_uri);
        if (role == GUI::ModelRole::Display) {
            switch (index.column()) {
            case 0:
                return p.name;
            case 1:
                return p.status_text();
            case 2:
                return p.device_uri;
            case 3:
                return p.location;
            case 4:
                return p.is_default ? "●" : "";
            }
        }
        return {};
    }

private:
    Vector<Printer> m_printers;
};

class JobModel final : public GUI::Model {
public:
    explicit JobModel(Vector<PrintJob> jobs)
        : m_jobs(move(jobs))
    {
    }

    virtual int row_count(GUI::ModelIndex const&) const override { return m_jobs.size(); }
    virtual int column_count(GUI::ModelIndex const& index) const override { return index.is_valid() ? 0 : 5; }

    virtual ErrorOr<String> column_name(int column) const override
    {
        switch (column) {
        case 0:
            return "任务"_string;
        case 1:
            return "打印机"_string;
        case 2:
            return "用户"_string;
        case 3:
            return "大小"_string;
        case 4:
            return "提交时间"_string;
        default:
            return "?"_string;
        }
    }

    virtual GUI::ModelIndex index(int row, int column, GUI::ModelIndex const&) const override
    {
        if (row < 0 || row >= (int)m_jobs.size())
            return {};
        return create_index(row, column, &m_jobs[row]);
    }

    virtual GUI::Variant data(GUI::ModelIndex const& index, GUI::ModelRole role) const override
    {
        if (index.row() < 0 || index.row() >= (int)m_jobs.size())
            return {};
        auto& j = m_jobs[index.row()];
        if (role == GUI::ModelRole::Display) {
            switch (index.column()) {
            case 0:
                return j.id;
            case 1:
                return j.printer;
            case 2:
                return j.user;
            case 3:
                return bytes_to_human(j.size_bytes);
            case 4:
                return j.submitted;
            }
        }
        return {};
    }

private:
    Vector<PrintJob> m_jobs;
};

} // namespace

// --- Add-printer dialog ---------------------------------------------------------

class AddPrinterDialog final : public GUI::Dialog {
    C_OBJECT(AddPrinterDialog)
public:
    ByteString printer_name() const { return m_name; }
    ByteString device_uri() const { return m_uri; }
    ByteString driver_id() const { return m_driver; }
    ByteString location() const { return m_location; }
    ByteString description() const { return m_description; }

private:
    AddPrinterDialog(GUI::Window* parent)
        : GUI::Dialog(parent, ScreenPosition::CenterWithinParent)
    {
        set_title("添加打印机"sv);
        resize(480, 260);

        auto main = set_main_widget<GUI::Widget>();
        main->set_layout<GUI::VerticalBoxLayout>(8, 6);
        main->set_fill_with_background_color(true);

        auto add_row = [main](StringView label_text) -> GUI::Widget& {
            auto& row = main->add<GUI::Widget>();
            row.set_layout<GUI::HorizontalBoxLayout>(4);
            auto label = String::from_utf8(label_text).release_value_but_fixme_should_propagate_errors();
            auto& gui_label = row.add<GUI::Label>(move(label));
            gui_label.set_fixed_width(64);
            return row;
        };

        auto& name_row = add_row("名称"sv);
        auto& name_edit = name_row.add<GUI::TextBox>();
        name_edit.set_placeholder("例如 office-laser"sv);

        m_device_uris = discover_devices();
        auto& uri_row = add_row("设备URI"sv);
        auto& uri_combo = uri_row.add<GUI::ComboBox>();
        uri_combo.set_only_allow_values_from_model(false);
        // The model keeps a reference to m_device_uris, so it must be a member.
        uri_combo.set_model(*GUI::ItemListModel<ByteString>::create(m_device_uris));
        if (!m_device_uris.is_empty())
            uri_combo.set_text(m_device_uris.first());

        m_driver_ids = { "everywhere"sv, "raw"sv };
        m_driver_labels = { "IPP Everywhere（自动检测 — 推荐）"sv, "raw（原样发送，不经过滤）"sv };
        for (auto const& entry : discover_drivers()) {
            auto offset = entry.find(' ');
            if (!offset.has_value())
                continue;
            m_driver_ids.append(entry.substring(0, offset.value()));
            m_driver_labels.append(entry.substring(offset.value() + 1));
        }
        auto& driver_row = add_row("驱动"sv);
        auto& driver_combo = driver_row.add<GUI::ComboBox>();
        driver_combo.set_only_allow_values_from_model(true);
        driver_combo.set_model(*GUI::ItemListModel<ByteString>::create(m_driver_labels));
        driver_combo.set_selected_index(0);
        m_driver_combo = &driver_combo;

        auto& location_row = add_row("位置"sv);
        auto& location_edit = location_row.add<GUI::TextBox>();
        location_edit.set_placeholder("可选"sv);

        auto& description_row = add_row("说明"sv);
        auto& description_edit = description_row.add<GUI::TextBox>();
        description_edit.set_placeholder("可选"sv);

        auto& buttons = main->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>(6);
        buttons.add_spacer();
        auto& add_button = buttons.add<GUI::Button>("添加"_string);
        add_button.set_fixed_width(88);
        auto& cancel_button = buttons.add<GUI::Button>("取消"_string);
        cancel_button.set_fixed_width(88);

        add_button.on_click = [this, &name_edit, &uri_combo, &location_edit, &description_edit](auto) {
            m_name = sanitize_queue_name(name_edit.text().trim_whitespace());
            if (m_name.is_empty()) {
                GUI::MessageBox::show_error(this, "请输入打印机名称。"sv);
                return;
            }
            m_uri = uri_combo.text().trim_whitespace();
            if (m_uri.is_empty() || !m_uri.contains("://"sv)) {
                GUI::MessageBox::show_error(this, "请输入有效的设备 URI（例如 ipp://…）。"sv);
                return;
            }
            m_driver = selected_driver_id();
            m_location = location_edit.text().trim_whitespace();
            m_description = description_edit.text().trim_whitespace();
            done(ExecResult::OK);
        };
        cancel_button.on_click = [this](auto) { done(ExecResult::Aborted); };
    }

    // CUPS queue names must be printable single tokens: keep [A-Za-z0-9._-],
    // turn everything else into '_' so names like "财务 打印机" still become a
    // valid queue.
    static ByteString sanitize_queue_name(StringView input)
    {
        StringBuilder builder;
        for (auto ch : input) {
            auto byte = static_cast<unsigned char>(ch);
            bool ok = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9') || ch == '.' || ch == '_' || ch == '-';
            builder.append(ok ? ch : '_');
        }
        auto sanitized = builder.to_byte_string();
        while (sanitized.starts_with('.') || sanitized.starts_with('_'))
            sanitized = sanitized.substring(1);
        return sanitized;
    }

    static Vector<ByteString> discover_devices()
    {
        Vector<ByteString> uris;
        auto result = run_cups("lpinfo", { "--timeout"sv, "2"sv, "-v"sv }, false);
        if (!result.spawned || result.exit_code != 0)
            return uris;
        for (auto raw : result.output.split_view('\n')) {
            auto line = raw.trim_whitespace();
            auto tokens = line.split_view(' ');
            if (tokens.size() != 2)
                continue;
            if (!tokens[1].contains("://"sv))
                continue; // unprobed backend names are not usable URIs
            uris.append(ByteString { tokens[1] });
        }
        return uris;
    }

    static Vector<ByteString> discover_drivers()
    {
        Vector<ByteString> entries;
        auto result = run_cups("lpinfo", { "-m"sv }, false);
        if (!result.spawned || result.exit_code != 0)
            return entries;
        for (auto raw : result.output.split_view('\n')) {
            auto line = raw.trim_whitespace();
            if (!line.is_empty())
                entries.append(ByteString { line });
        }
        return entries;
    }

    ByteString selected_driver_id() const
    {
        auto index = m_driver_combo ? m_driver_combo->selected_index() : 0;
        if (index >= m_driver_ids.size())
            return "everywhere";
        return m_driver_ids[index];
    }

    GUI::ComboBox* m_driver_combo { nullptr };
    Vector<ByteString> m_device_uris;
    Vector<ByteString> m_driver_ids;
    Vector<ByteString> m_driver_labels;
    ByteString m_name, m_uri, m_driver, m_location, m_description;
};

// --- Printer properties dialog ---------------------------------------------------

class PrinterPropertiesDialog final : public GUI::Dialog {
    C_OBJECT(PrinterPropertiesDialog)
public:
    PrinterPropertiesDialog(GUI::Window* parent, Printer printer)
        : GUI::Dialog(parent, ScreenPosition::CenterWithinParent)
        , m_printer(move(printer))
    {
        set_title(ByteString::formatted("打印机属性 — {}", m_printer.name));
        resize(640, 500);

        auto main = set_main_widget<GUI::Widget>();
        main->set_layout<GUI::VerticalBoxLayout>(6, 4);
        main->set_fill_with_background_color(true);

        auto& tabs = main->add<GUI::TabWidget>();

        // === Tab 1: 常规 =================================================
        auto& general = tabs.add_tab<GUI::Widget>("常规"_string);
        general.set_layout<GUI::VerticalBoxLayout>(10, 8);
        general.set_fill_with_background_color(true);

        auto add_row = [&general](StringView label_text) -> GUI::Widget& {
            auto& row = general.add<GUI::Widget>();
            row.set_layout<GUI::HorizontalBoxLayout>(6);
            auto label = String::from_utf8(label_text).release_value_but_fixme_should_propagate_errors();
            auto& gui_label = row.add<GUI::Label>(move(label));
            gui_label.set_fixed_width(72);
            return row;
        };

        auto& name_row = add_row("名称"sv);
        auto& name_label = name_row.add<GUI::Label>(String::from_utf8(m_printer.name.view()).release_value_but_fixme_should_propagate_errors());
        name_label.set_font(Gfx::FontDatabase::default_font().bold_variant());
        name_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& driver_row = add_row("驱动"sv);
        m_driver_label = &driver_row.add<GUI::Label>(""_string);
        m_driver_label->set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& info_row = add_row("说明"sv);
        m_info_edit = &info_row.add<GUI::TextBox>();
        m_info_edit->set_placeholder("可选"sv);

        auto& location_row = add_row("位置"sv);
        m_location_edit = &location_row.add<GUI::TextBox>();
        m_location_edit->set_placeholder("可选"sv);

        auto& uri_row = add_row("设备 URI"sv);
        m_uri_edit = &uri_row.add<GUI::TextBox>();

        auto& shared_row = add_row(""sv);
        m_shared_checkbox = &shared_row.add<GUI::CheckBox>("共享此打印机（供网络上的其他用户使用）"_string);

        auto& general_spacer = general.add<GUI::Widget>();
        general_spacer.set_fixed_height(1);

        // === Tab 2: 默认选项 ==============================================
        auto& options_tab = tabs.add_tab<GUI::Widget>("默认选项"_string);
        options_tab.set_layout<GUI::VerticalBoxLayout>(4, 4);
        options_tab.set_fill_with_background_color(true);

        auto& options_scroll = options_tab.add<GUI::ScrollableContainerWidget>();
        m_options_content = GUI::Widget::construct();
        options_scroll.set_widget(m_options_content);
        auto& options_rows = *m_options_content;
        options_rows.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10, 12, 6, 12 }, 4);
        options_rows.set_fill_with_background_color(true);
        options_rows.set_background_role(Gfx::ColorRole::Base);

        // === Tab 3: 高级 ==================================================
        auto& advanced = tabs.add_tab<GUI::Widget>("高级"_string);
        advanced.set_layout<GUI::VerticalBoxLayout>(10, 8);
        advanced.set_fill_with_background_color(true);

        auto& accepting_row = advanced.add<GUI::Widget>();
        accepting_row.set_layout<GUI::HorizontalBoxLayout>(6);
        auto& accepting_label = accepting_row.add<GUI::Label>(String {});
        accepting_label.set_fixed_width(72);
        m_accepting_checkbox = &accepting_row.add<GUI::CheckBox>("接受新打印任务（拒绝则用户提交的任务会被退回）"_string);

        auto& enabled_row = advanced.add<GUI::Widget>();
        enabled_row.set_layout<GUI::HorizontalBoxLayout>(6);
        auto& enabled_label = enabled_row.add<GUI::Label>(String {});
        enabled_label.set_fixed_width(72);
        m_enabled_checkbox = &enabled_row.add<GUI::CheckBox>("启用打印机（禁用则保留任务但暂不打印）"_string);

        auto& policy_row = advanced.add<GUI::Widget>();
        policy_row.set_layout<GUI::HorizontalBoxLayout>(6);
        auto& policy_label = policy_row.add<GUI::Label>("错误处理"_string);
        policy_label.set_fixed_width(72);
        m_error_policy_combo = &policy_row.add<GUI::ComboBox>();
        m_error_policy_combo->set_only_allow_values_from_model(true);
        m_error_policy_values = { "retry-job"sv, "abort-job"sv, "stop-printer"sv };
        m_error_policy_labels = { "重试任务（retry-job）"sv, "终止任务（abort-job）"sv, "停止打印机（stop-printer）"sv };
        m_error_policy_combo->set_model(*GUI::ItemListModel<ByteString>::create(m_error_policy_labels));
        m_error_policy_combo->set_fixed_width(280);

        auto& quota_header = advanced.add<GUI::Label>("每用户配额（每项 0 表示不限制）："_string);
        quota_header.set_fixed_height(20);

        auto add_quota_row = [&advanced](StringView label_text, StringView unit) -> GUI::TextBox& {
            auto& row = advanced.add<GUI::Widget>();
            row.set_layout<GUI::HorizontalBoxLayout>(6);
            auto label = String::from_utf8(label_text).release_value_but_fixme_should_propagate_errors();
            auto& gui_label = row.add<GUI::Label>(move(label));
            gui_label.set_fixed_width(150);
            auto& edit = row.add<GUI::TextBox>();
            edit.set_fixed_width(90);
            auto unit_label = String::from_utf8(unit).release_value_but_fixme_should_propagate_errors();
            auto& gui_unit = row.add<GUI::Label>(move(unit_label));
            gui_unit.set_text_alignment(Gfx::TextAlignment::CenterLeft);
            return edit;
        };
        m_quota_period_edit = &add_quota_row("配额周期"sv, "秒"sv);
        m_page_limit_edit = &add_quota_row("任务页数上限"sv, "页"sv);
        m_k_limit_edit = &add_quota_row("任务数据量上限"sv, "KB"sv);

        auto& advanced_spacer = advanced.add<GUI::Widget>();
        advanced_spacer.set_fixed_height(1);

        // === Bottom buttons ===============================================
        auto& buttons = main->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>(6);
        buttons.add_spacer();
        auto& ok_button = buttons.add<GUI::Button>("确定"_string);
        ok_button.set_fixed_width(88);
        ok_button.set_default(true);
        auto& cancel_button = buttons.add<GUI::Button>("取消"_string);
        cancel_button.set_fixed_width(88);
        auto& apply_button = buttons.add<GUI::Button>("应用"_string);
        apply_button.set_fixed_width(88);

        ok_button.on_click = [this](auto) {
            if (apply_changes())
                done(ExecResult::OK);
        };
        cancel_button.on_click = [this](auto) { done(ExecResult::Aborted); };
        apply_button.on_click = [this](auto) { apply_changes(); };

        reload();
    }

private:
    struct OptionRow {
        ByteString key;
        ByteString label;
        GUI::ComboBox* combo { nullptr };
        ByteString initial; // choice that was current when the dialog was (re)loaded
    };

    void rebuild_option_rows()
    {
        m_option_rows.clear();
        auto& rows = *m_options_content;
        for (auto& child : rows.child_widgets())
            rows.remove_child(child);

        auto options = query_queue_options(m_printer.name);
        m_current_options = move(options);

        if (m_current_options.is_empty()) {
            auto& label = rows.add<GUI::Label>("raw 队列（或无驱动选项）——该打印机没有可配置的驱动默认值。"_string);
            label.set_fixed_height(22);
            label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
            return;
        }
        for (auto const& option : m_current_options) {
            auto& row = rows.add<GUI::Widget>();
            row.set_layout<GUI::HorizontalBoxLayout>(6);
            row.set_fixed_height(26);
            auto label = String::from_utf8(option.label.view()).release_value_but_fixme_should_propagate_errors();
            auto& gui_label = row.add<GUI::Label>(move(label));
            gui_label.set_fixed_width(190);
            gui_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
            auto& combo = row.add<GUI::ComboBox>();
            combo.set_only_allow_values_from_model(true);
            combo.set_model(*GUI::ItemListModel<ByteString>::create(option.choices));
            combo.set_selected_index(static_cast<size_t>(option.current));
            combo.set_fixed_width(300);
            m_option_rows.append({ option.key, option.label, &combo, option.choices[static_cast<size_t>(option.current)] });
        }
        rows.invalidate_layout();
    }

    void reload()
    {
        auto attrs = query_ipp_attributes(m_printer.name);
        m_attrs = attrs;
        m_original = attrs;
        if (!attrs.valid) {
            m_driver_label->set_text("未知（无法读取打印机属性——ipptool/cups 是否可用？）"_string);
        } else {
            ByteString driver = attrs.make_and_model;
            if (driver.is_empty())
                driver = "raw / 未知";
            auto driver_text = String::from_utf8(driver.view()).release_value_but_fixme_should_propagate_errors();
            m_driver_label->set_text(move(driver_text));
        }
        auto info_text = String::from_utf8(attrs.info.view()).release_value_but_fixme_should_propagate_errors();
        auto location_text = String::from_utf8(attrs.location.view()).release_value_but_fixme_should_propagate_errors();
        auto uri_text = String::from_utf8(attrs.device_uri.view()).release_value_but_fixme_should_propagate_errors();
        m_info_edit->set_text(info_text);
        m_location_edit->set_text(location_text);
        m_uri_edit->set_text(uri_text);
        m_shared_checkbox->set_checked(attrs.shared);
        m_accepting_checkbox->set_checked(attrs.accepting);
        m_enabled_checkbox->set_checked(attrs.enabled);
        m_original_accepting = attrs.accepting;
        m_original_enabled = attrs.enabled;
        m_original_shared = attrs.shared;
        m_original_error_policy = attrs.error_policy;
        m_quota_period_edit->set_text(ByteString::formatted("{}", attrs.quota_period));
        m_page_limit_edit->set_text(ByteString::formatted("{}", attrs.page_limit));
        m_k_limit_edit->set_text(ByteString::formatted("{}", attrs.k_limit));
        int policy_index = 0;
        for (size_t i = 0; i < m_error_policy_values.size(); ++i) {
            if (m_error_policy_values[i] == attrs.error_policy)
                policy_index = static_cast<int>(i);
        }
        m_error_policy_combo->set_selected_index(static_cast<size_t>(policy_index));
        rebuild_option_rows();
    }

    bool apply_changes()
    {
        Vector<ByteString> error_lines;
        auto append_error = [&](StringView what, ToolResult const& result) {
            ByteString detail;
            if (!result.spawned)
                detail = "无法启动工具";
            else if (!result.error.trim_whitespace().is_empty())
                detail = result.error.trim_whitespace();
            else
                detail = ByteString::formatted("退出码 {}", result.exit_code);
            error_lines.append(ByteString::formatted("{}：{}", what, detail));
        };
        auto lpadmin = [&](StringView what, Vector<ByteString> args) {
            auto result = run_cups("lpadmin", args, true);
            if (!result.spawned || result.exit_code != 0)
                append_error(what, result);
        };
        ByteString name = m_printer.name;

        // 常规
        if (m_info_edit->text() != m_original.info)
            lpadmin("说明"sv, { "-p"sv, name, "-D"sv, m_info_edit->text() });
        if (m_location_edit->text() != m_original.location)
            lpadmin("位置"sv, { "-p"sv, name, "-L"sv, m_location_edit->text() });
        auto new_uri = m_uri_edit->text().trim_whitespace();
        if (new_uri != m_original.device_uri) {
            auto question = MUST(String::formatted("更改设备 URI 为「{}」？\n填错打印机将不可用。", new_uri));
            if (GUI::MessageBox::show(this, question, "更改设备 URI"sv, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo) == GUI::MessageBox::ExecResult::Yes)
                lpadmin("设备 URI"sv, { "-p"sv, name, "-v"sv, new_uri });
        }
        if (m_shared_checkbox->is_checked() != m_original_shared)
            lpadmin("共享"sv, { "-p"sv, name, "-o"sv, ByteString::formatted("printer-is-shared={}", m_shared_checkbox->is_checked() ? "true"sv : "false"sv) });

        // 默认选项（每个选项单独提交，避免一条命令里部分失败被吞掉）
        for (auto const& row : m_option_rows) {
            auto current = row.combo->text();
            if (current != row.initial)
                lpadmin(row.label, { "-p"sv, name, "-o"sv, ByteString::formatted("{}={}", row.key, current) });
        }

        // 高级
        if (m_accepting_checkbox->is_checked() != m_original_accepting) {
            auto result = run_cups(m_accepting_checkbox->is_checked() ? "cupsaccept" : "cupsreject", { name }, true);
            if (!result.spawned || result.exit_code != 0)
                append_error("接受/拒绝任务"sv, result);
        }
        if (m_enabled_checkbox->is_checked() != m_original_enabled) {
            auto result = run_cups(m_enabled_checkbox->is_checked() ? "cupsenable" : "cupsdisable", { name }, true);
            if (!result.spawned || result.exit_code != 0)
                append_error("启用/禁用打印机"sv, result);
        }
        auto selected_policy = static_cast<size_t>(m_error_policy_combo->selected_index());
        auto new_policy = selected_policy < m_error_policy_values.size() ? m_error_policy_values[selected_policy] : ByteString { "retry-job"sv };
        if (new_policy != m_original_error_policy)
            lpadmin("错误处理"sv, { "-p"sv, name, "-o"sv, ByteString::formatted("printer-error-policy={}", new_policy) });

        auto quota_text = m_quota_period_edit->text().trim_whitespace();
        auto page_text = m_page_limit_edit->text().trim_whitespace();
        auto k_text = m_k_limit_edit->text().trim_whitespace();
        int quota_period = quota_text.to_number<int>().value_or(-1);
        int page_limit = page_text.to_number<int>().value_or(-1);
        int k_limit = k_text.to_number<int>().value_or(-1);
        if (quota_text.is_empty() || page_text.is_empty() || k_text.is_empty()
            || quota_period < 0 || page_limit < 0 || k_limit < 0) {
            error_lines.append("配额项必须是非负整数（0 表示不限制）");
        } else {
            auto original_quota = m_original.quota_period;
            auto original_page = m_original.page_limit;
            auto original_k = m_original.k_limit;
            if (quota_period != original_quota)
                lpadmin("配额周期"sv, { "-p"sv, name, "-o"sv, ByteString::formatted("job-quota-period={}", quota_period) });
            if (page_limit != original_page)
                lpadmin("任务页数上限"sv, { "-p"sv, name, "-o"sv, ByteString::formatted("job-page-limit={}", page_limit) });
            if (k_limit != original_k)
                lpadmin("任务数据量上限"sv, { "-p"sv, name, "-o"sv, ByteString::formatted("job-k-limit={}", k_limit) });
        }

        if (!error_lines.is_empty()) {
            StringBuilder joined;
            for (size_t i = 0; i < error_lines.size(); ++i) {
                if (i != 0)
                    joined.append('\n');
                joined.append(error_lines[i]);
            }
            auto message = MUST(String::formatted("部分设置应用失败：\n{}", joined.to_byte_string()));
            GUI::MessageBox::show_error(this, message);
            reload();
            return false;
        }
        reload();
        return true;
    }

    Printer m_printer;
    IppAttributes m_attrs;
    IppAttributes m_original;
    bool m_original_shared { true };
    bool m_original_accepting { true };
    bool m_original_enabled { true };
    ByteString m_original_error_policy { "retry-job"sv };
    Vector<QueueOption> m_current_options;
    Vector<OptionRow> m_option_rows;

    GUI::Label* m_driver_label { nullptr };
    GUI::TextBox* m_info_edit { nullptr };
    GUI::TextBox* m_location_edit { nullptr };
    GUI::TextBox* m_uri_edit { nullptr };
    GUI::CheckBox* m_shared_checkbox { nullptr };
    GUI::CheckBox* m_accepting_checkbox { nullptr };
    GUI::CheckBox* m_enabled_checkbox { nullptr };
    GUI::ComboBox* m_error_policy_combo { nullptr };
    Vector<ByteString> m_error_policy_values;
    Vector<ByteString> m_error_policy_labels;
    GUI::TextBox* m_quota_period_edit { nullptr };
    GUI::TextBox* m_page_limit_edit { nullptr };
    GUI::TextBox* m_k_limit_edit { nullptr };
    RefPtr<GUI::Widget> m_options_content;
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("打印管理"sv);
    window->resize(760, 470);
    {
        auto printer_icon_or_error = GUI::Icon::try_create_default_icon("printer"sv);
        auto fallback_icon_or_error = GUI::Icon::try_create_default_icon("app-settings"sv);
        if (!printer_icon_or_error.is_error() && printer_icon_or_error.value().bitmap_for_size(16))
            window->set_icon(printer_icon_or_error.value().bitmap_for_size(16));
        else if (!fallback_icon_or_error.is_error())
            window->set_icon(fallback_icon_or_error.value().bitmap_for_size(16));
    }

    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_layout<GUI::VerticalBoxLayout>(4, 6);
    main_widget->set_fill_with_background_color(true);

    // ---- Tabs ------------------------------------------------------------------
    auto& tabs = main_widget->add<GUI::TabWidget>();

    // === Printers tab ============================================================
    auto& printers_tab = tabs.add_tab<GUI::Widget>("打印机"_string);
    printers_tab.set_layout<GUI::VerticalBoxLayout>(4, 6);
    printers_tab.set_fill_with_background_color(true);

    auto& printers_table = printers_tab.add<GUI::TableView>();
    printers_table.set_selection_mode(GUI::AbstractView::SelectionMode::SingleSelection);

    auto& printer_buttons = printers_tab.add<GUI::Widget>();
    printer_buttons.set_layout<GUI::HorizontalBoxLayout>(4);
    printer_buttons.set_fixed_height(28);
    auto& add_button = printer_buttons.add<GUI::Button>("添加…"_string);
    auto& test_button = printer_buttons.add<GUI::Button>("打印测试页"_string);
    auto& properties_button = printer_buttons.add<GUI::Button>("属性…"_string);
    auto& default_button = printer_buttons.add<GUI::Button>("设为默认"_string);
    auto& enable_button = printer_buttons.add<GUI::Button>("禁用"_string);
    auto& reject_button = printer_buttons.add<GUI::Button>("拒绝任务"_string);
    auto& delete_button = printer_buttons.add<GUI::Button>("删除"_string);
    printer_buttons.add_spacer();
    auto& refresh_button = printer_buttons.add<GUI::Button>("刷新"_string);

    // === Jobs tab ================================================================
    auto& jobs_tab = tabs.add_tab<GUI::Widget>("打印任务"_string);
    jobs_tab.set_layout<GUI::VerticalBoxLayout>(4, 6);
    jobs_tab.set_fill_with_background_color(true);

    auto& jobs_table = jobs_tab.add<GUI::TableView>();
    jobs_table.set_selection_mode(GUI::AbstractView::SelectionMode::SingleSelection);

    auto& job_buttons = jobs_tab.add<GUI::Widget>();
    job_buttons.set_layout<GUI::HorizontalBoxLayout>(4);
    job_buttons.set_fixed_height(28);
    auto& cancel_job_button = job_buttons.add<GUI::Button>("取消所选任务"_string);
    auto& cancel_all_button = job_buttons.add<GUI::Button>("取消该打印机全部任务"_string);
    job_buttons.add_spacer();
    auto& jobs_refresh_button = job_buttons.add<GUI::Button>("刷新"_string);

    auto& statusbar = main_widget->add<GUI::Statusbar>();

    // ---- State + behaviour --------------------------------------------------------
    Vector<Printer> printers;
    Vector<PrintJob> jobs;
    bool cups_available = true;

    auto selected_printer_index = [&]() -> Optional<int> {
        auto const& selection = printers_table.selection();
        if (selection.is_empty())
            return {};
        auto index = selection.first();
        if (!index.is_valid() || index.row() < 0 || index.row() >= (int)printers.size())
            return {};
        return index.row();
    };

    auto selected_printer_name = [&]() -> Optional<ByteString> {
        auto row = selected_printer_index();
        if (!row.has_value())
            return {};
        return printers[*row].name;
    };

    auto selected_printer = [&]() -> Printer const* {
        auto row = selected_printer_index();
        if (!row.has_value())
            return nullptr;
        return &printers[*row];
    };

    auto selected_job = [&]() -> PrintJob const* {
        auto const& selection = jobs_table.selection();
        if (selection.is_empty())
            return nullptr;
        auto index = selection.first();
        if (!index.is_valid() || index.row() < 0 || index.row() >= (int)jobs.size())
            return nullptr;
        return &jobs[index.row()];
    };

    auto update_buttons = [&] {
        auto const* printer = selected_printer();
        bool has_printer = printer != nullptr;
        test_button.set_enabled(has_printer);
        properties_button.set_enabled(has_printer);
        default_button.set_enabled(has_printer && !printer->is_default);
        enable_button.set_enabled(has_printer);
        reject_button.set_enabled(has_printer);
        delete_button.set_enabled(has_printer);
        if (has_printer) {
            enable_button.set_text(printer->disabled ? "启用"_string : "禁用"_string);
            reject_button.set_text(printer->rejecting ? "接受任务"_string : "拒绝任务"_string);
        }
        cancel_job_button.set_enabled(selected_job() != nullptr);
        cancel_all_button.set_enabled(selected_job() != nullptr || has_printer);
    };

    auto show_tool_error = [&](StringView action, ToolResult const& result) {
        ByteString detail;
        if (!result.spawned)
            detail = "无法启动 CUPS 工具（cups 是否已安装？）";
        else if (!result.error.trim_whitespace().is_empty())
            detail = result.error.trim_whitespace();
        else
            detail = ByteString::formatted("退出码 {}", result.exit_code);
        auto message = MUST(String::formatted("{}失败：\n{}", action, detail));
        GUI::MessageBox::show_error(window, message);
    };

    auto refresh_printers = [&] {
        auto keep = selected_printer_name();
        auto scheduler = Core::command("/usr/bin/lpstat", { "-r"sv }, {});
        cups_available = !scheduler.is_error()
            && buffer_text(scheduler.value().output).contains("running"sv);
        printers = query_printers();
        auto model = adopt_ref(*new PrinterModel(printers));
        printers_table.set_model(model);
        // HeaderView column sizing requires a model; set widths here.
        printers_table.set_column_width(0, 180); // 名称
        printers_table.set_column_width(1, 110); // 状态
        printers_table.set_column_width(2, 280); // 设备
        printers_table.set_column_width(3, 150); // 位置
        printers_table.set_column_width(4, 48);  // 默认
        if (keep.has_value()) {
            for (int row = 0; row < (int)printers.size(); ++row) {
                if (printers[row].name == *keep)
                    printers_table.selection().add(model->index(row, 0, {}));
            }
        }
    };

    auto refresh_jobs = [&] {
        Optional<ByteString> keep;
        if (auto const* job = selected_job())
            keep = job->id;
        jobs = query_jobs();
        auto model = adopt_ref(*new JobModel(jobs));
        jobs_table.set_model(model);
        jobs_table.set_column_width(0, 180); // 任务
        jobs_table.set_column_width(1, 140); // 打印机
        jobs_table.set_column_width(2, 110); // 用户
        jobs_table.set_column_width(3, 90);  // 大小
        jobs_table.set_column_width(4, 220); // 提交时间
        if (keep.has_value()) {
            for (int row = 0; row < (int)jobs.size(); ++row) {
                if (jobs[row].id == *keep)
                    jobs_table.selection().add(model->index(row, 0, {}));
            }
        }
    };

    auto refresh_all = [&] {
        refresh_printers();
        refresh_jobs();
        update_buttons();
        if (!cups_available) {
            statusbar.set_text("未找到运行中的 CUPS——请先安装并启动 cups 服务"_string);
            return;
        }
        if (printers.is_empty()) {
            statusbar.set_text("CUPS 已就绪，尚未配置任何打印机——点击「添加…」添加一台"_string);
            return;
        }
        auto text = MUST(String::formatted("共 {} 台打印机 · {} 个未完成任务 · 每 3 秒自动刷新", printers.size(), jobs.size()));
        statusbar.set_text(move(text));
    };

    // ---- Wiring ------------------------------------------------------------------
    add_button.on_click = [&](auto) {
        auto dialog = AddPrinterDialog::construct(window);
        if (dialog->exec() != GUI::Dialog::ExecResult::OK)
            return;
        Vector<ByteString> args { "-p"sv, dialog->printer_name(), "-E"sv, "-v"sv, dialog->device_uri(), "-m"sv, dialog->driver_id() };
        if (!dialog->location().is_empty())
            args.extend({ "-L"sv, dialog->location() });
        if (!dialog->description().is_empty())
            args.extend({ "-D"sv, dialog->description() });
        auto added_name = dialog->printer_name();
        auto result = run_cups("lpadmin", args, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("添加打印机"sv, result);
            return;
        }
        refresh_all();
        auto text = MUST(String::formatted("已添加打印机「{}」", added_name));
        statusbar.set_text(move(text));
    };

    properties_button.on_click = [&](auto) {
        auto row = selected_printer_index();
        if (!row.has_value())
            return;
        auto printer = printers[*row];
        auto dialog = PrinterPropertiesDialog::construct(window, move(printer));
        dialog->exec();
        refresh_all();
    };

    test_button.on_click = [&](auto) {
        auto name = selected_printer_name();
        if (!name.has_value())
            return;
        static ByteString const test_page = "/usr/share/cups/data/testprint";
        if (access(test_page.characters(), R_OK) != 0) {
            GUI::MessageBox::show_error(window, "找不到测试页文件 /usr/share/cups/data/testprint。"sv);
            return;
        }
        auto result = run_cups("lp", { "-d"sv, *name, test_page }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("打印测试页"sv, result);
            return;
        }
        auto output = result.output.trim_whitespace();
        auto message = MUST(String::formatted("测试页已发送到打印机「{}」。\n{}", *name, output));
        GUI::MessageBox::show(window, message, "打印测试页"sv);
        refresh_all();
    };

    default_button.on_click = [&](auto) {
        auto name = selected_printer_name();
        if (!name.has_value())
            return;
        auto result = run_cups("lpadmin", { "-d"sv, *name }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("设为默认"sv, result);
            return;
        }
        refresh_all();
        auto text = MUST(String::formatted("默认打印机已设为「{}」", *name));
        statusbar.set_text(move(text));
    };

    enable_button.on_click = [&](auto) {
        auto row = selected_printer_index();
        if (!row.has_value())
            return;
        auto name = printers[*row].name;
        bool currently_disabled = printers[*row].disabled;
        auto result = run_cups(currently_disabled ? "cupsenable" : "cupsdisable", { name }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error(currently_disabled ? "启用打印机"sv : "禁用打印机"sv, result);
            return;
        }
        refresh_all();
    };

    reject_button.on_click = [&](auto) {
        auto row = selected_printer_index();
        if (!row.has_value())
            return;
        auto name = printers[*row].name;
        bool currently_rejecting = printers[*row].rejecting;
        auto result = run_cups(currently_rejecting ? "cupsaccept" : "cupsreject", { name }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error(currently_rejecting ? "接受打印任务"sv : "拒绝打印任务"sv, result);
            return;
        }
        refresh_all();
    };

    delete_button.on_click = [&](auto) {
        auto name = selected_printer_name();
        if (!name.has_value())
            return;
        auto question = MUST(String::formatted("确定要删除打印机「{}」吗？\n此操作不可恢复。", *name));
        if (GUI::MessageBox::show(window, question, "删除打印机"sv, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo) != GUI::MessageBox::ExecResult::Yes)
            return;
        auto result = run_cups("lpadmin", { "-x"sv, *name }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("删除打印机"sv, result);
            return;
        }
        refresh_all();
    };

    cancel_job_button.on_click = [&](auto) {
        auto const* job = selected_job();
        if (!job)
            return;
        auto result = run_cups("cancel", { job->id }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("取消任务"sv, result);
            return;
        }
        refresh_all();
    };

    cancel_all_button.on_click = [&](auto) {
        Optional<ByteString> target;
        if (auto const* job = selected_job())
            target = job->printer;
        else if (auto name = selected_printer_name(); name.has_value())
            target = move(name);
        if (!target.has_value())
            return;
        auto question = MUST(String::formatted("确定要取消打印机「{}」上的全部任务吗？", *target));
        if (GUI::MessageBox::show(window, question, "取消全部任务"sv, GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo) != GUI::MessageBox::ExecResult::Yes)
            return;
        auto result = run_cups("cancel", { "-a"sv, *target }, true);
        if (!result.spawned || result.exit_code != 0) {
            show_tool_error("取消全部任务"sv, result);
            return;
        }
        refresh_all();
    };

    refresh_button.on_click = [&](auto) { refresh_all(); };
    jobs_refresh_button.on_click = [&](auto) { refresh_all(); };

    printers_table.on_selection_change = [&] { update_buttons(); };
    jobs_table.on_selection_change = [&] { update_buttons(); };

    auto refresh_timer = Core::Timer::create();
    refresh_timer->on_timeout = [&] { refresh_all(); };
    refresh_timer->start(3000);

    refresh_all();

    window->show();
    return app->exec();
}
