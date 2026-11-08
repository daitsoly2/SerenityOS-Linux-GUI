/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// serenity-print-service - the port's printing daemon.
//
// CUPS stays the system print *service* (queues, drivers, filters); this daemon
// is the session-side front-end that every Serenity GUI app talks to, so an app
// never spawns `lp` itself and the UI can ask "what printers exist" without
// knowing anything about CUPS. It listens on a Unix socket in the session
// runtime directory and speaks a tiny line protocol:
//
//   LIST
//     -> "OK <count>\n" followed by one printer name per line
//   PRINT <printer> <media> <orientation> <copies> <fit 0|1> <path>
//     -> "OK <lp output>"  /  "ERR <message>"
//   SETTINGS
//     -> "OK <media> <orientation> <fit> <copies>"  (global defaults)
//   SET <key> <value>
//     -> persists one default and replies with the new SETTINGS line
//   STATUS
//     -> "OK serenity-print-service/1 cups=<0|1>"
//
// Fields are tab separated; a path containing a tab is not supported (CUPS puts
// the job in the spool before it would matter). One request per connection.

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/RefPtr.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Command.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <Ports/LinuxGUI/Common/PortPrintGeometry.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

ByteString const lp_path = "/usr/bin/lp"sv;
ByteString const lpstat_path = "/usr/bin/lpstat"sv;

// Global defaults live in the service, not in each app: paper, orientation and
// "fit to page" should mean the same thing in ImageViewer, PDF viewer and any
// future print button.
struct Settings {
    ByteString media { "A4" };
    ByteString orientation { "portrait" };
    ByteString scaling { "fit" }; // fit | shrink | fill | original | custom
    int percent { 100 };
    bool fit_to_page { true };
    int copies { 1 };
};

ByteString settings_path()
{
    auto home = Core::StandardPaths::home_directory();
    if (home.is_empty())
        home = "/tmp";
    return ByteString::formatted("{}/.config/serenity/print.ini", home);
}

Settings load_settings()
{
    Settings settings;
    auto config = Core::ConfigFile::open(settings_path());
    if (config.is_error())
        return settings;

    auto const& file = *config.value();
    auto media = file.read_entry("Defaults", "Media").trim_whitespace();
    if (!media.is_empty())
        settings.media = media;
    auto orientation = file.read_entry("Defaults", "Orientation").trim_whitespace();
    if (orientation == "landscape"sv || orientation == "portrait"sv)
        settings.orientation = orientation;
    auto scaling = file.read_entry("Defaults", "Scaling").trim_whitespace();
    if (scaling == "fit"sv || scaling == "shrink"sv || scaling == "fill"sv
        || scaling == "original"sv || scaling == "custom"sv)
        settings.scaling = scaling;
    auto percent = file.read_num_entry("Defaults", "Percent", settings.percent);
    if (percent >= 10 && percent <= 400)
        settings.percent = percent;
    settings.fit_to_page = file.read_bool_entry("Defaults", "FitToPage", settings.fit_to_page);
    auto copies = file.read_num_entry("Defaults", "Copies", settings.copies);
    if (copies >= 1 && copies <= 99)
        settings.copies = copies;
    return settings;
}

void save_settings(Settings const& settings)
{
    auto directory = LexicalPath { settings_path() }.parent().string();
    (void)Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes);

    auto config = Core::ConfigFile::open(settings_path(), Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error()) {
        warnln("print-service: cannot write {}: {}", settings_path(), config.error());
        return;
    }
    auto& file = *config.value();
    file.write_entry("Defaults", "Media", settings.media);
    file.write_entry("Defaults", "Orientation", settings.orientation);
    file.write_entry("Defaults", "Scaling", settings.scaling);
    file.write_num_entry("Defaults", "Percent", settings.percent);
    file.write_bool_entry("Defaults", "FitToPage", settings.fit_to_page);
    file.write_num_entry("Defaults", "Copies", settings.copies);
    if (auto result = file.sync(); result.is_error())
        warnln("print-service: cannot save settings: {}", result.error());
}

ByteString settings_reply(Settings const& settings)
{
    return ByteString::formatted("OK\t{}\t{}\t{}\t{}\t{}\t{}\n",
        settings.media, settings.orientation, settings.scaling, settings.percent,
        settings.fit_to_page ? 1 : 0, settings.copies);
}

ByteString service_socket_path()
{
    if (char const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/serenity-print.sock", runtime);
    return ByteString::formatted("/tmp/serenity-print-{}.sock", getuid());
}

bool tool_installed(ByteString const& path)
{
    return access(path.characters(), X_OK) == 0;
}

Vector<ByteString> list_printers()
{
    Vector<ByteString> printers;
    if (!tool_installed(lpstat_path))
        return printers;

    auto result = Core::command(lpstat_path, { "-e"sv }, {});
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
// Page composition (images and PDFs)
//
// Every scaling mode is realised here, in the daemon, so the printed sheet is
// exactly what the print dialog preview showed. The picture is laid out on a
// canvas that has the sheet's portrait aspect ratio; the canvas is then sent to
// CUPS with fit-to-page, which maps it 1:1 onto the page, and CUPS rotates the
// portrait canvas to a landscape page when the user asked for landscape.
//
// Documents (PDFs) are rasterised with pdftoppm first, then each page is
// composed the same way, so "fill / shrink / custom" behave identically for a
// photo and for a multi-page report.
// ---------------------------------------------------------------------------

ByteString const pdftoppm_path = "/usr/bin/pdftoppm"sv;

struct PaperDef {
    char const* media;
    int width_mm;
    int height_mm;
};

Vector<PaperDef> paper_defs()
{
    // Must stay in sync with PortPrint::paper_sizes() in PortPrint.h.
    return Vector<PaperDef> {
        { "A4", 210, 297 },
        { "Letter", 216, 279 },
        { "10x15cm", 100, 150 },
        { "A5", 148, 210 },
    };
}

// Rasterises every page of a PDF to a temporary PNG and returns the file paths.
// Returns an empty vector if pdftoppm is missing or the file is not a PDF.
Vector<ByteString> rasterize_pdf(ByteString const& pdf_path)
{
    Vector<ByteString> pages;
    if (!tool_installed(pdftoppm_path))
        return pages;

    auto runtime = getenv("XDG_RUNTIME_DIR");
    // pdftoppm appends "-<n>.png" to the prefix, so the prefix must NOT end in
    // a separator of its own or the names gain a double dash.
    ByteString base = ByteString::formatted("{}/serenity-print-pdf-{}",
        runtime && *runtime ? ByteString { runtime } : ByteString { "/tmp" }, getpid());
    auto result = Core::command(pdftoppm_path, { "-png"sv, "-r"sv, "150"sv, pdf_path, base }, {});
    if (result.is_error() || result.value().exit_code != 0)
        return pages;

    // pdftoppm writes <base>-1.png, <base>-2.png, ...
    for (int n = 1; n < 2000; ++n) {
        auto file = ByteString::formatted("{}-{}.png", base, n);
        if (access(file.characters(), R_OK) != 0)
            break;
        pages.append(move(file));
    }
    return pages;
}

// Composes one raster image (a photograph or a rasterised PDF page) onto a
// portrait paper canvas and writes it to a temporary PNG. Returns {} on failure.
ByteString compose_one_page(ByteString const& source_path, int paper_width_mm, int paper_height_mm, StringView scaling, int percent, int index)
{
    if (!Gfx::Bitmap::is_path_a_supported_image_format(source_path))
        return {};
    auto loaded = Gfx::Bitmap::load_from_file(source_path);
    if (loaded.is_error())
        return {};
    auto source = loaded.release_value();
    if (source->width() <= 0 || source->height() <= 0)
        return {};

    auto canvas_size = PortPrint::page_canvas_size(paper_width_mm, paper_height_mm);
    auto placement = PortPrint::compute_placement(source->width(), source->height(), canvas_size, scaling, percent);

    auto page_or_error = Gfx::Bitmap::create(source->format(), canvas_size);
    if (page_or_error.is_error())
        return {};
    auto page = page_or_error.release_value();
    page->fill(Color::White);

    RefPtr<Gfx::Bitmap const> image = placement.rotated ? PortPrint::rotate_quarter_turn(*source) : RefPtr<Gfx::Bitmap const> { source };
    if (!image)
        return {};

    Gfx::Painter painter(page);
    painter.add_clip_rect(Gfx::IntRect { 0, 0, canvas_size.width(), canvas_size.height() });
    painter.draw_scaled_bitmap(placement.destination, *image, image->rect());

    auto encoded = Gfx::PNGWriter::encode(*page);
    if (encoded.is_error())
        return {};

    auto runtime = getenv("XDG_RUNTIME_DIR");
    // A per-call counter keeps the temp names unique even when several single
    // page jobs share the same pid and page index.
    static unsigned long s_temp_counter = 0;
    ByteString destination = ByteString::formatted("{}/serenity-print-page-{}-{}-{}.png",
        runtime && *runtime ? ByteString { runtime } : ByteString { "/tmp" }, getpid(), index, s_temp_counter++);
    auto file = Core::File::open(destination, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return {};
    if (file.value()->write_until_depleted(encoded.value()).is_error())
        return {};
    return destination;
}

// Returns the file(s) to print - one entry per physical page. The daemon owns
// the page layout, so images and PDFs are composed here. Empty means "print the
// original path as-is" (text, an unsupported format, or a missing tool).
// When set, composed / rasterised temp files are left on disk (under
// XDG_RUNTIME_DIR) instead of being unlinked, so a developer can inspect the
// exact page the daemon hands to CUPS. Off by default.
bool keep_temp_files()
{
    auto const* value = getenv("SERENITY_PRINT_KEEP_TEMP");
    return value && *value;
}

Vector<ByteString> prepare_print_files(ByteString const& path, ByteString const& media, ByteString const& scaling, int percent)
{
    Vector<ByteString> out;

    int pw = 210, ph = 297; // default A4
    for (auto const& size : paper_defs()) {
        if (size.media == media) {
            pw = size.width_mm;
            ph = size.height_mm;
            break;
        }
    }

    // "original" is printed at 100% by CUPS directly; composing would let
    // fit-to-page rescale and defeat the point.
    if (scaling == "original"sv)
        return out;

    auto ext = LexicalPath { path }.extension().to_byte_string().to_lowercase();
    if (ext == "pdf"sv) {
        auto raster = rasterize_pdf(path);
        if (raster.is_empty())
            return out; // no pdftoppm: let CUPS render the PDF itself
        int index = 0;
        for (auto const& png : raster) {
            auto composed = compose_one_page(png, pw, ph, scaling, percent, index++);
            if (composed.is_empty()) {
                out.append(png); // keep the raw raster as a fallback
            } else {
                out.append(move(composed));
                if (!keep_temp_files())
                    unlink(png.characters()); // raw raster no longer needed
            }
        }
        return out;
    }

    if (Gfx::Bitmap::is_path_a_supported_image_format(path)) {
        auto composed = compose_one_page(path, pw, ph, scaling, percent, 0);
        if (!composed.is_empty())
            out.append(move(composed));
        // else: composition failed - caller prints the original image.
    }
    return out;
}

ByteString submit_job(ByteString const& printer, ByteString const& media, ByteString const& orientation, ByteString const& scaling, int percent, int copies, bool fit_to_page, ByteString const& path, bool& ok)
{
    if (!tool_installed(lp_path)) {
        ok = false;
        return "未安装 CUPS 客户端(lp)"sv;
    }

    Vector<ByteString> files = prepare_print_files(path, media, scaling, percent);
    bool composed = !files.is_empty();
    if (files.is_empty())
        files.append(path);

    Vector<ByteString> arguments;
    arguments.append("-d"sv);
    arguments.append(printer);
    if (copies > 1) {
        arguments.append("-n"sv);
        arguments.append(ByteString::number(copies));
    }
    if (!media.is_empty()) {
        arguments.append("-o"sv);
        arguments.append(ByteString::formatted("media={}", media));
    }
    if (composed) {
        // The canvas already has the sheet's aspect and the picture laid out, so
        // just map it 1:1 onto the page. Orientation is expressed by letting
        // CUPS rotate the portrait canvas to a landscape page.
        if (orientation == "landscape"sv) {
            arguments.append("-o"sv);
            arguments.append("landscape"sv);
        }
        arguments.append("-o"sv);
        arguments.append("fit-to-page"sv);
    } else {
        if (orientation == "landscape"sv) {
            arguments.append("-o"sv);
            arguments.append("landscape"sv);
        }
        if (scaling == "original"sv) {
            // 100%: let the driver clip whatever does not fit the sheet.
            arguments.append("-o"sv);
            arguments.append("scaling=100"sv);
        } else if (fit_to_page) {
            arguments.append("-o"sv);
            arguments.append("fit-to-page"sv);
        }
    }
    for (auto const& f : files)
        arguments.append(f);

    auto result = Core::command(lp_path, arguments, {});
    // Composed / rasterised temp files have been spooled by now.
    if (composed && !keep_temp_files()) {
        for (auto const& f : files)
            unlink(f.characters());
    }
    if (result.is_error()) {
        ok = false;
        return ByteString::formatted("无法执行 lp：{}", result.error());
    }
    if (result.value().exit_code != 0) {
        ok = false;
        auto const& error = result.value().error;
        auto message = StringView { error.data(), error.size() }.trim_whitespace();
        return message.is_empty() ? ByteString { "lp 返回失败" } : ByteString { message };
    }

    ok = true;
    auto const& output = result.value().output;
    return StringView { output.data(), output.size() }.trim_whitespace().to_byte_string();
}

// ---------------------------------------------------------------------------
// Desktop notifications
//
// The compositor already serves org.freedesktop.Notifications on the session
// bus, so the daemon reports "printing / done / failed" there instead of each
// app popping its own message box.
// ---------------------------------------------------------------------------

void notify(ByteString const& summary, ByteString const& body, int timeout_ms = 6000)
{
    DBusError error;
    dbus_error_init(&error);
    auto* connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!connection) {
        dbus_error_free(&error);
        return;
    }
    dbus_connection_set_exit_on_disconnect(connection, false);

    auto* message = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (!message) {
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        return;
    }

    DBusMessageIter arguments;
    dbus_message_iter_init_append(message, &arguments);
    char const* app_name = "serenity-print-service";
    dbus_uint32_t replaces_id = 0;
    char const* app_icon = "printer";
    char const* summary_text = summary.characters();
    char const* body_text = body.characters();

    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &app_name);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_UINT32, &replaces_id);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &app_icon);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &summary_text);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &body_text);

    DBusMessageIter actions;
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "s", &actions);
    dbus_message_iter_close_container(&arguments, &actions);
    DBusMessageIter hints;
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &hints);
    dbus_message_iter_close_container(&arguments, &hints);
    dbus_int32_t expire_timeout = timeout_ms;
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_INT32, &expire_timeout);

    auto* reply = dbus_connection_send_with_reply_and_block(connection, message, 3000, &error);
    if (reply)
        dbus_message_unref(reply);
    else
        dbus_error_free(&error);

    dbus_message_unref(message);
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
}

// Jobs the daemon has queued and not yet seen leave the CUPS queue, so it can
// tell the user when printing finished instead of leaving a "printing..." toast
// on screen forever.
struct TrackedJob {
    ByteString id;
    ByteString name;
    ByteString printer;
};

Vector<TrackedJob> g_tracked_jobs;

Vector<ByteString> active_job_ids()
{
    Vector<ByteString> ids;
    if (!tool_installed(lpstat_path))
        return ids;
    auto result = Core::command(lpstat_path, { "-W"sv, "not-completed"sv, "-o"sv }, {});
    if (result.is_error() || result.value().exit_code != 0)
        return ids;
    auto const& output = result.value().output;
    StringView text { output.data(), output.size() };
    for (auto line : text.split_view('\n')) {
        auto trimmed = line.trim_whitespace();
        if (trimmed.is_empty())
            continue;
        if (auto space = trimmed.find(' '); space.has_value())
            ids.append(trimmed.substring_view(0, space.value()).to_byte_string());
        else
            ids.append(trimmed.to_byte_string());
    }
    return ids;
}

void poll_tracked_jobs()
{
    if (g_tracked_jobs.is_empty())
        return;

    auto active = active_job_ids();
    for (size_t i = g_tracked_jobs.size(); i-- > 0;) {
        auto const& job = g_tracked_jobs[i];
        bool still_queued = false;
        for (auto const& id : active) {
            if (id == job.id) {
                still_queued = true;
                break;
            }
        }
        if (still_queued)
            continue;
        notify(ByteString::formatted("打印完成：{}", job.name),
            ByteString::formatted("作业 {} 已从打印机 {} 的队列中移出。", job.id, job.printer),
            5000);
        g_tracked_jobs.remove(i);
    }
}

ByteString extract_job_id(StringView lp_output)
{
    // `lp` prints "request id is opttest-12 (1 file(s))".
    auto marker = lp_output.find("request id is "sv);
    if (!marker.has_value())
        return {};
    auto rest = lp_output.substring_view(marker.value() + 15);
    if (auto space = rest.find(' '); space.has_value())
        rest = rest.substring_view(0, space.value());
    return rest.trim_whitespace().to_byte_string();
}

ByteString handle_request(StringView request, bool& ok)
{
    ok = true;
    if (request == "LIST"sv) {
        auto printers = list_printers();
        StringBuilder builder;
        builder.appendff("OK\t{}\n", printers.size());
        for (auto const& printer : printers)
            builder.appendff("{}\n", printer);
        return builder.to_byte_string();
    }

    if (request == "STATUS"sv) {
        auto cups = tool_installed(lp_path) && tool_installed(lpstat_path);
        return ByteString::formatted("OK\tserenity-print-service/1 cups={}\n", cups ? 1 : 0);
    }

    if (request == "SETTINGS"sv)
        return settings_reply(load_settings());

    if (request.starts_with("SET\t"sv)) {
        auto fields = request.substring_view(4).split_view('\t');
        if (fields.size() < 2) {
            ok = false;
            return "ERR\t参数不足\n"sv;
        }
        auto key = fields[0];
        auto value = fields[1];
        auto settings = load_settings();
        if (key == "media"sv) {
            settings.media = value.to_byte_string();
        } else if (key == "orientation"sv) {
            if (value != "portrait"sv && value != "landscape"sv) {
                ok = false;
                return "ERR\t方向只能是 portrait 或 landscape\n"sv;
            }
            settings.orientation = value.to_byte_string();
        } else if (key == "scaling"sv) {
            if (value != "fit"sv && value != "shrink"sv && value != "fill"sv
                && value != "original"sv && value != "custom"sv) {
                ok = false;
                return "ERR\t缩放只能是 fit/shrink/fill/original/custom\n"sv;
            }
            settings.scaling = value.to_byte_string();
        } else if (key == "percent"sv) {
            auto percent = value.to_byte_string().to_number<int>().value_or(100);
            settings.percent = percent < 10 ? 10 : (percent > 400 ? 400 : percent);
        } else if (key == "fit"sv) {
            settings.fit_to_page = value == "1"sv || value == "true"sv;
        } else if (key == "copies"sv) {
            auto copies = value.to_byte_string().to_number<int>().value_or(1);
            settings.copies = copies < 1 ? 1 : (copies > 99 ? 99 : copies);
        } else {
            ok = false;
            return "ERR\t未知设置项\n"sv;
        }
        save_settings(settings);
        return settings_reply(settings);
    }

    if (request.starts_with("PRINT\t"sv)) {
        auto fields = request.substring_view(6).split_view('\t');
        if (fields.size() < 8) {
            ok = false;
            return "ERR\t参数不足\n"sv;
        }
        auto printer = fields[0].to_byte_string();
        auto media = fields[1].to_byte_string();
        auto orientation = fields[2].to_byte_string();
        auto scaling = fields[3].to_byte_string();
        auto percent = fields[4].to_byte_string().to_number<int>().value_or(100);
        auto copies = fields[5].to_byte_string().to_number<int>().value_or(1);
        auto fit = fields[6] == "1"sv;
        auto path = fields[7].to_byte_string();
        // Re-join in case the path itself contained a tab.
        for (size_t i = 8; i < fields.size(); ++i)
            path = ByteString::formatted("{}\t{}", path, fields[i]);

        bool submitted = false;
        auto message = submit_job(printer, media, orientation, scaling, percent, copies < 1 ? 1 : copies, fit, path, submitted);
        ok = submitted;

        auto name = LexicalPath { path }.basename();
        if (submitted) {
            auto job_id = extract_job_id(message);
            notify(ByteString::formatted("正在打印：{}", name),
                ByteString::formatted("打印机 {} · 作业 {}", printer, job_id.is_empty() ? ByteString { "(未知)" } : job_id),
                4000);
            if (!job_id.is_empty())
                g_tracked_jobs.append({ job_id, name, printer });
        } else {
            // Failures are reported here, not by the calling app: one place, one
            // wording, and the app can just close its dialog.
            notify(ByteString::formatted("打印失败：{}", name), message, 8000);
        }
        return ByteString::formatted("{}\t{}\n", submitted ? "OK"sv : "ERR"sv, message);
    }

    ok = false;
    return "ERR\t未知请求\n"sv;
}

int create_listening_socket(ByteString const& path)
{
    auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.length() >= sizeof(address.sun_path)) {
        warnln("print-service: socket path too long: {}", path);
        close(fd);
        return -1;
    }
    memcpy(address.sun_path, path.characters(), path.length());

    // A stale socket from a crashed session would make bind() fail with
    // EADDRINUSE; the path lives in XDG_RUNTIME_DIR, so it is ours to replace.
    unlink(path.characters());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    chmod(path.characters(), 0600);

    if (listen(fd, 8) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

void serve_client(int client)
{
    timeval timeout { 5, 0 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    StringBuilder incoming;
    char buffer[1024];
    while (true) {
        auto nread = read(client, buffer, sizeof(buffer));
        if (nread <= 0)
            break;
        incoming.append(StringView { buffer, static_cast<size_t>(nread) });
        if (incoming.string_view().find('\n').has_value())
            break;
        if (incoming.length() > 8192)
            break;
    }

    auto request = incoming.string_view().trim_whitespace();
    if (request.is_empty())
        return;

    bool ok = true;
    auto reply = handle_request(request, ok);
    (void)write(client, reply.characters(), reply.length());
}

}

int main()
{
    // Messages are per-request; a dead client must not take the daemon down.
    signal(SIGPIPE, SIG_IGN);

    auto socket_path = service_socket_path();
    auto listener = create_listening_socket(socket_path);
    if (listener < 0)
        return 1;

    outln("serenity-print-service: listening on {}", socket_path);

    while (true) {
        pollfd descriptor { listener, POLLIN, 0 };
        // Woken up every 2s so queued jobs can be checked for completion.
        auto ready = poll(&descriptor, 1, g_tracked_jobs.is_empty() ? -1 : 2000);
        if (ready == 0) {
            poll_tracked_jobs();
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        auto client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            continue;
        }
        serve_client(client);
        close(client);
    }

    unlink(socket_path.characters());
    return 0;
}
