/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Printing front-end shared by every GUI app in the port.
//
// Design: CUPS is the real print service (queues, drivers, filters). The port
// adds *one* small session daemon, `serenity-print-service`, that wraps it, and
// every app talks to that daemon over a Unix socket instead of spawning `lp`
// itself. Two reasons:
//   * the UI only needs "which printers exist" + "submit this file", and that
//     logic should not be copy-pasted into ImageViewer, PDF viewer, editors...;
//   * a single place decides how a job is submitted (media, copies, fit), so a
//     later switch to libcups or a sandboxed helper touches one binary.
//
// The direct `lp` path is kept only as a fallback for the (rare) case where the
// daemon is not running; apps never call it deliberately.
//
// Header-only on purpose, like AppIcons.h / ExplorerMenus.h: it is compiled into
// each app that shows a print button, so no new library target / link change is
// needed in the per-app CMake targets.

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Command.h>
#include <LibCore/System.h>
#include <LibCore/File.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Palette.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <Ports/LinuxGUI/Common/PortPrintGeometry.h>

namespace PortPrint {

inline ByteString const& lp_path()
{
    static ByteString path { "/usr/bin/lp" };
    return path;
}

inline ByteString const& lpstat_path()
{
    static ByteString path { "/usr/bin/lpstat" };
    return path;
}

inline ByteString const& service_protocol_version()
{
    static ByteString version { "serenity-print-service/1" };
    return version;
}

inline ByteString socket_path()
{
    if (char const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/serenity-print.sock", runtime);
    return ByteString::formatted("/tmp/serenity-print-{}.sock", getuid());
}

// ---------------------------------------------------------------------------
// Daemon client
// ---------------------------------------------------------------------------

struct ServiceReply {
    bool connected { false };
    bool ok { false };
    ByteString text;
};

inline ServiceReply talk_to_service(StringView request)
{
    ServiceReply reply;

    auto path = socket_path();
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.length() >= sizeof(address.sun_path))
        return reply;
    memcpy(address.sun_path, path.characters(), path.length());

    auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return reply;
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return reply;
    }
    reply.connected = true;

    timeval timeout { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    auto payload = ByteString::formatted("{}\n", request);
    if (write(fd, payload.characters(), payload.length()) != static_cast<ssize_t>(payload.length())) {
        close(fd);
        return reply;
    }

    StringBuilder incoming;
    char buffer[1024];
    while (true) {
        auto nread = read(fd, buffer, sizeof(buffer));
        if (nread <= 0)
            break;
        incoming.append(StringView { buffer, static_cast<size_t>(nread) });
        // LIST replies with a header line followed by one line per printer, so
        // keep reading until the peer closes (it closes right after replying).
    }
    close(fd);

    auto text = incoming.string_view().trim_whitespace();
    if (text.is_empty())
        return reply;
    reply.ok = text.starts_with("OK"sv);
    if (!reply.ok && text.starts_with("ERR\t"sv))
        text = text.substring_view(4);
    else if (text.starts_with("OK\t"sv))
        text = text.substring_view(3);
    reply.text = text.to_byte_string();
    return reply;
}

inline bool service_running()
{
    return talk_to_service("STATUS"sv).connected;
}

// ---------------------------------------------------------------------------
// Direct CUPS helpers (fallback only)
// ---------------------------------------------------------------------------

inline Vector<ByteString> printers_from_lpstat()
{
    Vector<ByteString> printers;
    if (Core::System::access(lpstat_path(), X_OK).is_error())
        return printers;

    auto result = Core::command(lpstat_path(), { "-e"sv }, {});
    if (result.is_error() || result.value().exit_code != 0)
        return printers;

    auto const& output = result.value().output;
    StringView text { output.data(), output.size() };
    for (auto line : text.split_view('\n')) {
        auto name = line.trim_whitespace();
        if (!name.is_empty())
            printers.append(name.to_byte_string());
    }
    return printers;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

inline Vector<ByteString> available_printers()
{
    auto reply = talk_to_service("LIST"sv);
    if (reply.connected && reply.ok) {
        // First line is the count; every following line is a printer name.
        Vector<ByteString> printers;
        bool first = true;
        for (auto line : reply.text.split_view('\n')) {
            if (first) {
                first = false;
                continue;
            }
            auto name = line.trim_whitespace();
            if (!name.is_empty())
                printers.append(name.to_byte_string());
        }
        if (!printers.is_empty())
            return printers;
        // A live daemon with an empty list is authoritative: no printers.
        if (reply.text.trim_whitespace() == "0"sv)
            return printers;
    }
    return printers_from_lpstat();
}

// Global defaults owned by the daemon (paper / orientation / copies / fit).
// Every app shows the same values; changing them in one print dialog changes
// them for all apps, which is what "system print settings" should mean.
struct Settings {
    ByteString media { "A4" };
    ByteString orientation { "portrait" };
    ByteString scaling { "fit" }; // fit | shrink | fill | original | custom
    int percent { 100 };
    int copies { 1 };
    bool fit_to_page { true };
};

inline Settings parse_settings(StringView text)
{
    Settings settings;
    auto fields = text.split_view('\t');
    if (fields.size() >= 6) {
        settings.media = fields[0].to_byte_string();
        settings.orientation = fields[1].to_byte_string();
        settings.scaling = fields[2].to_byte_string();
        settings.percent = fields[3].to_byte_string().to_number<int>().value_or(100);
        settings.fit_to_page = fields[4] == "1"sv;
        settings.copies = fields[5].to_byte_string().to_number<int>().value_or(1);
    }
    return settings;
}

inline Settings settings()
{
    auto reply = talk_to_service("SETTINGS"sv);
    if (reply.connected && reply.ok)
        return parse_settings(reply.text);
    return {};
}

// Persists one default; returns the settings that are now in effect.
inline Settings update_setting(StringView key, StringView value)
{
    auto reply = talk_to_service(ByteString::formatted("SET\t{}\t{}", key, value));
    if (reply.connected && reply.ok)
        return parse_settings(reply.text);
    return settings();
}

struct Request {
    ByteString printer;
    ByteString media { "A4" };
    ByteString orientation { "portrait" };
    ByteString scaling { "fit" };
    int percent { 100 };
    int copies { 1 };
    bool fit_to_page { true };
};

// Returns the error text on failure, nothing on success.
inline Optional<ByteString> print_file(StringView path, Request const& request)
{
    auto reply = talk_to_service(ByteString::formatted("PRINT\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
        request.printer, request.media, request.orientation, request.scaling, request.percent,
        request.copies, request.fit_to_page ? 1 : 0, path));
    if (reply.connected)
        return reply.ok ? Optional<ByteString> {} : Optional<ByteString> { reply.text };

    // Fallback: the daemon is not running, talk to CUPS directly.
    if (Core::System::access(lp_path(), X_OK).is_error())
        return ByteString { "打印服务未运行，且未安装 CUPS 客户端(lp)" };

    Vector<ByteString> arguments;
    arguments.append("-d"sv);
    arguments.append(request.printer);
    if (request.copies > 1) {
        arguments.append("-n"sv);
        arguments.append(ByteString::number(request.copies));
    }
    if (!request.media.is_empty()) {
        arguments.append("-o"sv);
        arguments.append(ByteString::formatted("media={}", request.media));
    }
    if (request.orientation == "landscape"sv) {
        arguments.append("-o"sv);
        arguments.append("landscape"sv);
    }
    if (request.fit_to_page) {
        arguments.append("-o"sv);
        arguments.append("fit-to-page"sv);
    }
    arguments.append(ByteString { path });

    auto result = Core::command(lp_path(), arguments, {});
    if (result.is_error())
        return ByteString::formatted("无法执行 lp：{}", result.error());
    if (result.value().exit_code != 0) {
        auto const& error = result.value().error;
        auto message = StringView { error.data(), error.size() }.trim_whitespace();
        return message.is_empty() ? ByteString { "lp 返回失败" } : ByteString { message };
    }
    return {};
}

// ---------------------------------------------------------------------------
// Shared dialog
// ---------------------------------------------------------------------------

struct PaperSize {
    char const* media;
    char const* label;
    int width_mm;
    int height_mm;
};

inline Vector<PaperSize> const& paper_sizes()
{
    static Vector<PaperSize> const sizes {
        { "A4", "A4 (210×297 mm)", 210, 297 },
        { "Letter", "US Letter (216×279 mm)", 216, 279 },
        { "10x15cm", "照片 10×15 cm", 100, 150 },
        { "A5", "A5 (148×210 mm)", 148, 210 },
    };
    return sizes;
}

// Dry-run of the sheet the printer will produce: white paper at the chosen
// aspect ratio with the image fitted inside its printable area. CUPS does not
// render previews - page layout belongs to the application.
class PreviewWidget final : public GUI::Widget {
    C_OBJECT(PreviewWidget)
public:
    void set_image(RefPtr<Gfx::Bitmap const> image)
    {
        m_image = move(image);
        m_rotated.clear();
        update();
    }
    void set_paper(PaperSize paper)
    {
        m_paper = paper;
        update();
    }
    void set_orientation(ByteString orientation)
    {
        if (m_orientation == orientation)
            return;
        m_orientation = move(orientation);
        m_rotated.clear();
        update();
    }
    void set_scaling(ByteString scaling, int percent)
    {
        if (m_scaling == scaling && m_percent == percent)
            return;
        m_scaling = move(scaling);
        m_percent = percent;
        update();
    }

protected:
    void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());

        auto area = rect().shrunken(6, 4);
        if (area.is_empty())
            return;

        // Landscape really is a rotated sheet: the page loses its height and
        // gains width.
        bool landscape = m_orientation == "landscape"sv;
        int page_width_mm = landscape ? m_paper.height_mm : m_paper.width_mm;
        int page_height_mm = landscape ? m_paper.width_mm : m_paper.height_mm;

        float aspect = static_cast<float>(page_width_mm) / page_height_mm;
        int page_height = area.height();
        int page_width = static_cast<int>(page_height * aspect);
        if (page_width > area.width()) {
            page_width = area.width();
            page_height = static_cast<int>(page_width / aspect);
        }
        Gfx::IntRect page { 0, 0, page_width, page_height };
        page.center_within(area);

        painter.fill_rect(page, Color::White);
        painter.draw_rect(page, Color::from_rgb(0x808080));

        if (!m_image)
            return;

        // Lay the picture out on the sheet's own canvas with the same geometry
        // the daemon uses to build the real page, then map canvas pixels to
        // widget pixels.
        auto canvas = page_canvas_size(page_width_mm, page_height_mm);
        auto placement = compute_placement(m_image->width(), m_image->height(), canvas, m_scaling, m_percent);

        RefPtr<Gfx::Bitmap const> source = m_image;
        if (placement.rotated) {
            if (!m_rotated)
                m_rotated = rotate_quarter_turn(*m_image);
            if (m_rotated)
                source = m_rotated;
            else
                placement.rotated = false;
        }

        auto ratio = static_cast<float>(page.width()) / canvas.width();
        Gfx::IntRect destination {
            page.x() + static_cast<int>(placement.destination.x() * ratio),
            page.y() + static_cast<int>(placement.destination.y() * ratio),
            max(1, static_cast<int>(placement.destination.width() * ratio)),
            max(1, static_cast<int>(placement.destination.height() * ratio)),
        };
        // fill / custom may overflow the printable area: clip to the sheet.
        painter.add_clip_rect(page);
        painter.draw_scaled_bitmap(destination, *source, source->rect());
    }

private:
    PreviewWidget() = default;

    RefPtr<Gfx::Bitmap const> m_image;
    RefPtr<Gfx::Bitmap const> m_rotated;
    PaperSize m_paper { "A4", "A4", 210, 297 };
    ByteString m_orientation { "portrait" };
    ByteString m_scaling { "fit" };
    int m_percent { 100 };
};

// Opens the print dialog for `path` and submits the job on OK. Any GUI app can
// call this; it owns the whole interaction.
inline void show_dialog(GUI::Window* parent, ByteString const& path, StringView title = "打印"sv)
{
    if (path.is_empty()) {
        GUI::MessageBox::show(parent, "当前没有选中的文件，无法打印。"sv, title,
            GUI::MessageBox::Type::Information);
        return;
    }

    if (!service_running()) {
        GUI::MessageBox::show(parent,
            ByteString::formatted("打印服务(serenity-print-service)没有运行。\n请重启桌面会话，或手动执行 serenity-print-service。"),
            title, GUI::MessageBox::Type::Error);
        return;
    }

    auto printers = available_printers();
    if (printers.is_empty()) {
        GUI::MessageBox::show(parent,
            "没有可用的打印机。\n请先在「设置 → 打印机和其它硬件 → 管理打印机和打印任务」中添加。"sv,
            title, GUI::MessageBox::Type::Error);
        return;
    }

    RefPtr<Gfx::Bitmap const> preview_image;
    if (Gfx::Bitmap::is_path_a_supported_image_format(path)) {
        if (auto loaded = Gfx::Bitmap::load_from_file(path); !loaded.is_error())
            preview_image = loaded.release_value();
    }

    auto dialog = GUI::Dialog::construct(parent);
    dialog->set_title(title);

    auto& root = *dialog->set_main_widget<GUI::Widget>();
    // Without this the dialog body stays transparent (black on the port) and the
    // dark label text is invisible.
    root.set_fill_with_background_color(true);
    root.set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12, 12, 12, 12 }, 8);

    auto global = settings();

    auto& preview = root.add<PreviewWidget>();
    // Fixed, not preferred: with a Grow/preferred height the dialog packs to the
    // layout minimum and the sheet collapses to a 30px strip.
    preview.set_fixed_height(250);
    preview.set_image(preview_image);

    auto make_row = [&](StringView label_text) -> GUI::Widget& {
        auto& row = root.add<GUI::Widget>();
        row.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 8);
        auto& label = row.add<GUI::Label>(MUST(String::from_utf8(label_text)));
        label.set_preferred_width(56);
        return row;
    };

    auto& printer_row = make_row("打印机："sv);
    auto& printer_combo = printer_row.add<GUI::ComboBox>();
    // ItemListModel keeps a *reference* to the vector: it must outlive the
    // dialog, and this local does (exec() blocks until the dialog closes).
    auto printer_items = printers;
    printer_combo.set_model(*GUI::ItemListModel<ByteString>::create(printer_items));
    printer_combo.set_preferred_width(GUI::SpecialDimension::Grow);
    printer_combo.set_text(printer_items.first());

    auto& orientation_row = make_row("方向："sv);
    auto& orientation_combo = orientation_row.add<GUI::ComboBox>();
    Vector<ByteString> orientation_labels { "纵向"sv, "横向"sv };
    orientation_combo.set_model(*GUI::ItemListModel<ByteString>::create(orientation_labels));
    orientation_combo.set_preferred_width(120);
    orientation_combo.set_text(global.orientation == "landscape"sv ? orientation_labels[1] : orientation_labels[0]);
    preview.set_orientation(global.orientation);
    auto& orientation_spacer = orientation_row.add<GUI::Widget>();
    orientation_spacer.set_preferred_width(GUI::SpecialDimension::Grow);

    auto& paper_row = make_row("纸张："sv);
    auto& paper_combo = paper_row.add<GUI::ComboBox>();
    Vector<ByteString> paper_labels;
    for (auto const& size : paper_sizes())
        paper_labels.append(ByteString { size.label });
    paper_combo.set_model(*GUI::ItemListModel<ByteString>::create(paper_labels));
    paper_combo.set_preferred_width(GUI::SpecialDimension::Grow);
    {
        // Start from the daemon's global default, falling back to the first row.
        size_t selected = 0;
        for (size_t i = 0; i < paper_sizes().size(); ++i) {
            if (global.media == paper_sizes()[i].media) {
                selected = i;
                break;
            }
        }
        paper_combo.set_text(paper_labels[selected]);
    }

    auto& copies_row = make_row("份数："sv);
    auto& copies_spinbox = copies_row.add<GUI::SpinBox>();
    copies_spinbox.set_range(1, 99);
    copies_spinbox.set_value(global.copies);
    copies_spinbox.set_preferred_width(80);
    auto& copies_spacer = copies_row.add<GUI::Widget>();
    copies_spacer.set_preferred_width(GUI::SpecialDimension::Grow);

    auto& scaling_row = make_row("缩放："sv);
    auto& scaling_combo = scaling_row.add<GUI::ComboBox>();
    // The five modes every desktop print dialog offers.
    Vector<ByteString> scaling_labels {
        "适应页面（整张可见）"sv,
        "缩小到可打印区域（只缩不放）"sv,
        "填充页面（裁掉边缘）"sv,
        "原始大小 100%"sv,
        "自定义百分比"sv,
    };
    scaling_combo.set_model(*GUI::ItemListModel<ByteString>::create(scaling_labels));
    scaling_combo.set_preferred_width(GUI::SpecialDimension::Grow);
    auto& percent_spinbox = scaling_row.add<GUI::SpinBox>();
    percent_spinbox.set_range(10, 400);
    percent_spinbox.set_value(global.percent);
    percent_spinbox.set_preferred_width(72);

    auto scaling_index_for = [&](ByteString const& mode) -> size_t {
        if (mode == "shrink"sv)
            return 1;
        if (mode == "fill"sv)
            return 2;
        if (mode == "original"sv)
            return 3;
        if (mode == "custom"sv)
            return 4;
        return 0;
    };
    scaling_combo.set_text(scaling_labels[scaling_index_for(global.scaling)]);
    preview.set_scaling(global.scaling, global.percent);

    auto& remember_checkbox = root.add<GUI::CheckBox>(MUST(String::from_utf8("保存为所有应用的默认设置"sv)));
    remember_checkbox.set_checked(true);

    auto current_paper = [&]() -> PaperSize const& {
        auto index = static_cast<int>(paper_combo.selected_index());
        if (index < 0 || index >= static_cast<int>(paper_sizes().size()))
            index = 0;
        return paper_sizes()[index];
    };

    paper_combo.on_change = [&](auto const&, auto const&) {
        preview.set_paper(current_paper());
    };
    orientation_combo.on_change = [&](auto const&, auto const&) {
        preview.set_orientation(orientation_combo.selected_index() == 1 ? "landscape"sv : "portrait"sv);
    };
    auto current_scaling = [&]() -> StringView {
        switch (static_cast<int>(scaling_combo.selected_index())) {
        case 1:
            return "shrink"sv;
        case 2:
            return "fill"sv;
        case 3:
            return "original"sv;
        case 4:
            return "custom"sv;
        default:
            return "fit"sv;
        }
    };
    auto sync_scaling = [&] {
        auto mode = current_scaling();
        // The percentage only matters for "custom"; keep it enabled-looking by
        // disabling the field instead of hiding it.
        percent_spinbox.set_enabled(mode == "custom"sv);
        preview.set_scaling(mode.to_byte_string(), percent_spinbox.value());
    };
    scaling_combo.on_change = [&](auto const&, auto const&) { sync_scaling(); };
    percent_spinbox.on_change = [&](int) { sync_scaling(); };
    sync_scaling();
    preview.set_paper(current_paper());

    auto& buttons = root.add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 8);
    buttons.set_preferred_height(26);
    auto& button_spacer = buttons.add<GUI::Widget>();
    button_spacer.set_preferred_width(GUI::SpecialDimension::Grow);
    auto& cancel_button = buttons.add<GUI::Button>(MUST(String::from_utf8("取消"sv)));
    cancel_button.set_preferred_width(84);
    auto& print_button = buttons.add<GUI::Button>(MUST(String::from_utf8("打印"sv)));
    print_button.set_preferred_width(84);
    print_button.set_default(true);

    cancel_button.on_click = [&](unsigned) { dialog->done(GUI::Dialog::ExecResult::Cancel); };
    print_button.on_click = [&](unsigned) { dialog->done(GUI::Dialog::ExecResult::OK); };

    // Size after the widget tree exists: resizing earlier lets set_main_widget()
    // re-pack to the layout's preferred size, and a Grow preview collapses.
    dialog->resize(520, 540);
    dialog->set_resizable(false);

    if (dialog->exec() != GUI::Dialog::ExecResult::OK)
        return;

    Request request;
    request.printer = printer_combo.text();
    if (request.printer.is_empty())
        request.printer = printer_items.first();
    request.media = ByteString { current_paper().media };
    request.orientation = orientation_combo.selected_index() == 1 ? "landscape"sv : "portrait"sv;
    request.copies = copies_spinbox.value();
    request.scaling = current_scaling().to_byte_string();
    request.percent = percent_spinbox.value();
    // Every mode is realised by composing the page ourselves, so the sheet gets
    // exactly what the preview shows and the driver only has to map it 1:1.
    request.fit_to_page = true;

    if (remember_checkbox.is_checked()) {
        // These are daemon-owned defaults, so every app's print dialog starts
        // from the same paper / orientation next time.
        (void)update_setting("media"sv, request.media);
        (void)update_setting("orientation"sv, request.orientation);
        (void)update_setting("scaling"sv, request.scaling);
        (void)update_setting("percent"sv, ByteString::number(request.percent));
        (void)update_setting("fit"sv, request.fit_to_page ? "1"sv : "0"sv);
        (void)update_setting("copies"sv, ByteString::number(request.copies));
    }

    // The daemon (serenity-print-service) realises every scaling mode itself:
    // it composes the page and submits it to CUPS, so the app only hands over
    // the original file plus the chosen options.
    (void)print_file(path, request);
}

}
