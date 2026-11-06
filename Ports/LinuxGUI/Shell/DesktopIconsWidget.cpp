/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Win9x-style desktop icons for the Linux GUI port.
//
// A full-screen WindowType::Desktop window that paints the wallpaper plus a
// column of icons, and launches the matching app on double click. It is one
// half of the desktop shell: the taskbar (start menu, window list, clock, tray)
// lives in the same process, see Shell/main.cpp.
//
// The desktop is DIRECTORY-DRIVEN: it is a folder of shortcut files, exactly
// like a Win9x desktop. Both Serenity `.af` and freedesktop `.desktop` files
// are recognized, so Linux apps (and wine-installed ones) drop in for free:
//
//   ~/Desktop/                          user shortcuts (auto-created)
//   /usr/share/serenity/desktop/        system-provided defaults
//
//   [App]                      or      [Desktop Entry]
//   Name=Calculator                    Name=Calculator
//   Executable=/bin/Calculator         Exec=serenity-calculator
//   Icon=/res/icons/32x32/...          Icon=calculator
//
// Right-click -> "New" writes a shortcut file into ~/Desktop; "Remove icon"
// deletes the matching shortcut file.

#include "DesktopIconsWidget.h"
#include "AppCatalog.h"
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/TemporaryChange.h>
#include <LibCore/Command.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibDesktop/Launcher.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/DragOperation.h>
#include <LibGUI/Event.h>
#include <LibGUI/FileIconProvider.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGUI/UITranslation.h>
#include <LibURL/URL.h>
#include <Ports/LinuxGUI/Common/Translation.h>
#include <Ports/LinuxGUI/Common/WinApps.h>
#include <Ports/LinuxGUI/Common/ArchiveOps.h>
#include <Ports/LinuxGUI/Common/ExplorerEntries.h>
#include <Ports/LinuxGUI/Common/ExplorerMenus.h>
#include <Ports/LinuxGUI/Common/hostify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int ICON_SIZE = 32;
constexpr int CELL_WIDTH = 84;
constexpr int CELL_HEIGHT = 76;
constexpr int MARGIN_X = 10;
constexpr int MARGIN_Y = 10;
constexpr int MAX_ROWS = 6;

ByteString translated(StringView key, StringView fallback)
{
    return PortTranslation::tr("Desktop"sv, key, fallback);
}

// What the compatibility launcher can run: native Windows binaries and the
// script formats Windows apps ship with.
bool is_windows_binary(ByteString const& path)
{
    for (auto suffix : { ".exe"sv, ".msi"sv, ".bat"sv, ".cmd"sv }) {
        if (path.ends_with(suffix))
            return true;
    }
    return false;
}

// Return the desktop directory, creating it if absent.
ByteString desktop_directory()
{
    auto dir = Core::StandardPaths::desktop_directory();
    if (dir.is_empty())
        dir = ByteString::formatted("{}/Desktop", Core::StandardPaths::home_directory());
    MUST(Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes));
    return dir;
}

// System-provided shortcuts. They live inside the install prefix
// (<prefix>/share/desktop) so the runtime stays in one directory; the old
// /usr/share location is still honored for pre-prefix installs.
ByteString system_desktop_directory()
{
    auto candidate = ByteString::formatted("{}/share/desktop", PortHostify::install_prefix());
    if (FileSystem::is_directory(candidate))
        return candidate;
    return ByteString { "/usr/share/serenity/desktop" };
}

// File templates behind "New > ...". Same layout as the desktop shortcuts:
// <prefix>/share/templates ships with the port, ~/Templates is the user's own
// (and overrides a system template with the same file name).
ByteString system_templates_directory()
{
    auto candidate = ByteString::formatted("{}/share/templates", PortHostify::install_prefix());
    if (FileSystem::is_directory(candidate))
        return candidate;
    return ByteString { "/usr/share/serenity/templates" };
}

ByteString user_templates_directory()
{
    return ByteString::formatted("{}/Templates", Core::StandardPaths::home_directory());
}

// --- Memory cache ---------------------------------------------------------

// System locations are never moved onto the desktop: dragging /usr or /res
// there would take the system apart. Everything under the home directory (and
// removable media) is fair game.
bool is_protected_path(StringView path)
{
    static constexpr StringView PROTECTED_PREFIXES[] = {
        "/bin"sv, "/boot"sv, "/dev"sv, "/etc"sv, "/lib"sv, "/lib64"sv,
        "/proc"sv, "/sbin"sv, "/sys"sv, "/usr"sv, "/var"sv, "/res"sv,
    };

    for (auto prefix : PROTECTED_PREFIXES) {
        if (path.starts_with(prefix) && (path.length() == prefix.length() || path[prefix.length()] == '/'))
            return true;
    }

    // The Serenity install prefix (<prefix>, usually /serenity) is system data.
    auto install_prefix = PortHostify::install_prefix();
    if (path.starts_with(install_prefix) && (path.length() == install_prefix.length() || path[install_prefix.length()] == '/'))
        return true;

    return false;
}

// Available RAM in kB, straight out of /proc/meminfo. 0 when unreadable.
u64 available_memory_kb()
{
    auto file = Core::File::open("/proc/meminfo"sv, Core::File::OpenMode::Read);
    if (file.is_error())
        return 0;
    auto content = file.value()->read_until_eof();
    if (content.is_error())
        return 0;

    auto bytes = content.release_value();
    for (auto line : StringView { bytes.bytes() }.split_view('\n')) {
        if (!line.starts_with("MemAvailable:"sv))
            continue;
        auto value = line.substring_view("MemAvailable:"sv.length()).trim_whitespace();
        return value.to_number<u64>().value_or(0);
    }
    return 0;
}

// Drop the kernel's page cache, dentries and inodes. Writing 3 to
// /proc/sys/vm/drop_caches needs CAP_SYS_ADMIN, so a non-root session simply
// skips it instead of failing.
bool drop_memory_caches()
{
    auto file = Core::File::open("/proc/sys/vm/drop_caches"sv, Core::File::OpenMode::Write);
    if (file.is_error())
        return false;
    auto written = file.value()->write_some(ReadonlyBytes { "3\n", 2 });
    if (written.is_error() || written.value() != 2)
        return false;
    (void)file.value()->close();
    return true;
}

}

// Parse a freedesktop .desktop file into a Shortcut.
Optional<DesktopIconsWidget::Shortcut> DesktopIconsWidget::parse_desktop_file(ByteString const& path)
{
    auto config = Core::ConfigFile::open(path);
    if (config.is_error())
        return {};
    auto const& cfg = *config.value();
    auto name = cfg.read_entry("Desktop Entry", "Name");
    auto exec = cfg.read_entry("Desktop Entry", "Exec");
    if (name.is_empty() || exec.is_empty())
        return {};
    if (cfg.read_bool_entry("Desktop Entry", "NoDisplay", false))
        return {};

    // Strip exec's field codes (e.g. "foo %U" -> "foo").
    auto exec_clean = exec;
    if (auto percent = exec_clean.find('%'); percent.has_value())
        exec_clean = exec_clean.substring(0, percent.value()).trim_whitespace();

    Shortcut s;
    s.name = name;
    s.exec = exec_clean;
    s.icon_path = cfg.read_entry("Desktop Entry", "Icon");
    s.path = cfg.read_entry("Desktop Entry", "Path");
    s.source_file = path;
    return s;
}

// Parse a Serenity .af file into a Shortcut.
Optional<DesktopIconsWidget::Shortcut> DesktopIconsWidget::parse_af_file(ByteString const& path)
{
    auto config = Core::ConfigFile::open(path);
    if (config.is_error())
        return {};
    auto const& cfg = *config.value();
    auto name = cfg.read_entry("App", "Name");
    auto exec = cfg.read_entry("App", "Executable");
    if (name.is_empty() || exec.is_empty())
        return {};
    Shortcut s;
    s.name = name;
    s.exec = exec;
    s.icon_path = cfg.read_entry("App", "Icon");
    s.source_file = path;
    return s;
}

// Scan a directory for .af / .desktop shortcut files and collect them.
void DesktopIconsWidget::scan_shortcuts(ByteString const& directory, Vector<Shortcut>& out)
{
    Core::DirIterator it(directory, Core::DirIterator::SkipDots);
    if (it.has_error())
        return;
    while (it.has_next()) {
        auto name = it.next_path();
        if (name.ends_with(".af"sv)) {
            if (auto s = parse_af_file(ByteString::formatted("{}/{}", directory, name)); s.has_value())
                out.append(s.release_value());
        } else if (name.ends_with(".desktop"sv)) {
            if (auto s = parse_desktop_file(ByteString::formatted("{}/{}", directory, name)); s.has_value())
                out.append(s.release_value());
        }
    }
}

void DesktopIconsWidget::load_shortcuts_into(Vector<Shortcut> const& shortcuts)
{
    for (auto const& s : shortcuts) {
        Entry entry;
        entry.exec = s.exec;
        entry.icon_path = s.icon_path;
        entry.path = s.path;
        entry.source_file = s.source_file;
        entry.is_shortcut = true;
        entry.label = MUST(String::from_byte_string(s.name));
        // Shortcut names come from the .desktop/.af file ("Calculator"); run
        // them through the same UI table the menus use.
        entry.label = GUI::translate_ui(entry.label);
        // '&' marks a mnemonic in menus; on a desktop icon label it would show
        // literally ("&Sound Player"), so strip it here.
        if (auto bytes = entry.label.to_byte_string(); bytes.contains('&'))
            entry.label = MUST(String::from_byte_string(bytes.replace("&"sv, ""sv, AK::ReplaceMode::All)));
        entry.icon = AppCatalog::the().icon_for_path(entry.icon_path, ICON_SIZE);
        m_entries.append(move(entry));
    }
}

// Everything else in ~/Desktop is a real file or folder: show it with its file
// type icon so dropping a document on the desktop behaves like a desktop.
void DesktopIconsWidget::load_plain_files_into(ByteString const& directory)
{
    Core::DirIterator it(directory, Core::DirIterator::SkipDots);
    if (it.has_error())
        return;

    while (it.has_next()) {
        auto name = it.next_path();
        if (name.ends_with(".af"sv) || name.ends_with(".desktop"sv))
            continue;

        auto path = ByteString::formatted("{}/{}", directory, name);
        Entry entry;
        entry.label = MUST(String::from_byte_string(name));
        entry.path = path;
        entry.source_file = path;
        entry.is_shortcut = false;

        auto icon = GUI::FileIconProvider::icon_for_path(path);
        // Entry keeps the bitmap, not the Icon, and icon_for_path() may build a
        // throw-away Icon (image thumbnails, symlink overlays) - clone so the
        // bitmap outlives the Icon.
        if (auto const* bitmap = icon.bitmap_for_size(ICON_SIZE)) {
            if (auto clone = bitmap->clone(); !clone.is_error())
                entry.icon = clone.release_value();
        }
        m_entries.append(move(entry));
    }
}

void DesktopIconsWidget::load_entries()
{
    // Reloading re-lays-out the icon list, so any outstanding selection indices
    // may now point at the wrong icon. Drop it to stay consistent.
    clear_selection();
    m_entries.clear();

    Vector<Shortcut> shortcuts;
    // System-provided defaults first, then the user's desktop folder
    // (user entries override the system ones by same source basename).
    scan_shortcuts(system_desktop_directory(), shortcuts);
    scan_shortcuts(desktop_directory(), shortcuts);

    // Deduplicate by basename of the source file (user wins over system).
    HashMap<ByteString, size_t> seen;
    Vector<Shortcut> merged;
    for (auto& s : shortcuts) {
        auto key = LexicalPath::basename(s.source_file);
        if (auto it = seen.find(key); it != seen.end()) {
            merged[it->value] = move(s);
        } else {
            seen.set(key, merged.size());
            merged.append(move(s));
        }
    }

    load_shortcuts_into(merged);
    load_plain_files_into(desktop_directory());
    layout_entries();

    dbgln("shell: desktop shows {} icons", m_entries.size());
}

// --- Icon positions --------------------------------------------------------

ByteString DesktopIconsWidget::desktop_config_path()
{
    auto directory = Core::StandardPaths::config_directory();
    if (directory.is_empty())
        directory = ByteString::formatted("{}/.config", Core::StandardPaths::home_directory());
    return ByteString::formatted("{}/Desktop.ini", directory);
}

void DesktopIconsWidget::load_positions(HashMap<ByteString, Gfx::IntPoint>& positions)
{
    auto config = Core::ConfigFile::open(desktop_config_path());
    if (config.is_error())
        return;

    auto& file = *config.value();
    for (auto const& key : file.keys("Positions"sv)) {
        auto value = file.read_entry("Positions"sv, key);
        auto parts = value.split(',');
        if (parts.size() != 2)
            continue;
        auto x = parts[0].trim_whitespace().to_number<int>();
        auto y = parts[1].trim_whitespace().to_number<int>();
        if (x.has_value() && y.has_value())
            positions.set(key, { x.value(), y.value() });
    }
}

void DesktopIconsWidget::save_position(ByteString const& key, Gfx::IntPoint position)
{
    auto path = desktop_config_path();
    if (!FileSystem::exists(path)) {
        if (auto result = Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes); result.is_error()) {
            warnln("shell: cannot create {}: {}", LexicalPath::dirname(path), result.error());
            return;
        }
        auto file = Core::File::open(path, Core::File::OpenMode::Write);
        if (file.is_error()) {
            warnln("shell: cannot create {}: {}", path, file.error());
            return;
        }
    }

    auto config = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error()) {
        warnln("shell: cannot write {}: {}", path, config.error());
        return;
    }
    config.value()->write_entry("Positions"sv, key, ByteString::formatted("{},{}", position.x(), position.y()));
    if (auto result = config.value()->sync(); result.is_error())
        warnln("shell: cannot save icon positions: {}", result.error());
}

void DesktopIconsWidget::clear_positions()
{
    auto config = Core::ConfigFile::open(desktop_config_path(), Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error())
        return;
    config.value()->remove_group("Positions"sv);
    (void)config.value()->sync();
}

void DesktopIconsWidget::layout_entries()
{
    HashMap<ByteString, Gfx::IntPoint> positions;
    load_positions(positions);

    // Grid cells taken by icons the user placed by hand, so the automatic
    // layout below does not drop another icon on top of them.
    // No HashSet in this tree, and the icon count is tiny.
    HashMap<int, bool> taken;
    auto cell_key = [](Gfx::IntPoint position) {
        auto column = max(0, (position.x() - MARGIN_X + CELL_WIDTH / 2) / CELL_WIDTH);
        auto row = max(0, (position.y() - MARGIN_Y + CELL_HEIGHT / 2) / CELL_HEIGHT);
        return row * 1000 + column;
    };

    for (auto& entry : m_entries) {
        auto it = positions.find(LexicalPath::basename(entry.source_file));
        if (it == positions.end())
            continue;
        entry.rect = { it->value, { CELL_WIDTH - 8, CELL_HEIGHT - 6 } };
        taken.set(cell_key(it->value), true);
    }

    size_t next_cell = 0;
    for (auto& entry : m_entries) {
        if (!entry.rect.is_empty())
            continue;
        // Column-major, like the Win9x desktop: fill a column, then go right.
        for (;;) {
            auto column = static_cast<int>(next_cell) / MAX_ROWS;
            auto row = static_cast<int>(next_cell) % MAX_ROWS;
            ++next_cell;
            auto key = row * 1000 + column;
            if (taken.contains(key))
                continue;
            taken.set(key, true);
            entry.rect = { MARGIN_X + column * CELL_WIDTH, MARGIN_Y + row * CELL_HEIGHT, CELL_WIDTH - 8, CELL_HEIGHT - 6 };
            break;
        }
    }
}

// Write a native Serenity .af shortcut into the desktop directory.
//
// User-created shortcuts are .af rather than freedesktop .desktop: .af is what
// the rest of the port reads (AppCatalog, DesktopIconsWidget::parse_af_file,
// the Start menu shortcut manager), so a shortcut made on the desktop can be
// copied to ~/.local/share/start-menu unchanged. Both icon keys are emitted
// because the desktop scanner reads "Icon" while LibDesktop::AppFile reads
// "IconPath".
void DesktopIconsWidget::write_shortcut_file(ByteString const& name, ByteString const& exec, ByteString const& icon_path)
{
    auto directory = desktop_directory();
    // Sanitize the name into a filename.
    StringBuilder filename;
    for (auto ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '&')
            filename.append('_');
        else
            filename.append(ch);
    }
    auto base = filename.to_byte_string().trim_whitespace();
    if (base.is_empty())
        base = "Untitled"sv;
    auto path = ByteString::formatted("{}/{}.af", directory, base);
    // Avoid clobbering: append a number if the file exists.
    int n = 1;
    while (FileSystem::exists(path)) {
        path = ByteString::formatted("{}/{}.{}.af", directory, base, n++);
    }

    auto content = ByteString::formatted(
        "[App]\n"
        "Name={}\n"
        "Executable={}\n"
        "Icon={}\n"
        "IconPath={}\n"
        "Category=Utilities\n",
        name, exec, icon_path, icon_path);
    if (auto f = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !f.is_error()) {
        (void)f.value()->write_until_depleted(content.bytes());
    } else {
        warnln("shell: cannot write shortcut {}: {}", path, f.error());
    }
}

RefPtr<Gfx::Bitmap const> DesktopIconsWidget::wallpaper()
{
    if (m_wallpaper_loaded)
        return m_wallpaper;
    m_wallpaper_loaded = true;

    // DisplaySettings persists the wallpaper through ConfigServer (domain
    // "WindowManager"), while older Linux-port sessions kept it in
    // [Background] Path= of /etc/WindowServer.ini. GUI::Desktop::wallpaper_path()
    // resolves both, so the desktop and the settings dialog finally agree.
    auto path = GUI::Desktop::the().wallpaper_path();
    m_wallpaper_path = path;
    m_wallpaper.clear();
    if (path.is_empty())
        return nullptr;

    auto bitmap = Gfx::Bitmap::load_from_file(path, 1);
    if (bitmap.is_error()) {
        warnln("shell: cannot load wallpaper {}: {}", path, bitmap.error());
        return nullptr;
    }
    m_wallpaper = bitmap.release_value();
    return m_wallpaper;
}

// A DisplaySettings "Apply" writes to ConfigServer (Wallpaper) and to the
// WindowServer config (Mode/Color) from another process. There is no push
// notification for the desktop, so poll those three values twice a second and
// repaint only when something actually changed. This replaces the old
// load-once-and-cache-forever behaviour that made wallpaper changes invisible.
void DesktopIconsWidget::reload_wallpaper_if_needed()
{
    ByteString path = GUI::Desktop::the().wallpaper_path();

    ByteString mode = "Fill"sv;
    ByteString color;
    if (auto config = Core::ConfigFile::open(ByteString { "/etc/WindowServer.ini" }); !config.is_error()) {
        mode = config.value()->read_entry("Background", "Mode", "Center");
        color = config.value()->read_entry("Background", "Color", "");
    }

    if (path == m_wallpaper_path && mode == m_wallpaper_mode && color == m_background_color)
        return;

    m_wallpaper_path = path;
    m_wallpaper_mode = mode;
    m_background_color = color;
    m_wallpaper_loaded = false;
    m_wallpaper.clear();
    update();
}

void DesktopIconsWidget::ensure_wallpaper_watcher()
{
    if (m_wallpaper_timer)
        return;

    // Seed the mode/colour/path once so the very first paint is correct.
    reload_wallpaper_if_needed();

    m_wallpaper_timer = Core::Timer::create_repeating(500, [this] {
        reload_wallpaper_if_needed();
    });
    m_wallpaper_timer->start();
}

// Watch ~/Desktop so files created elsewhere (e.g. a "compress" that writes a
// .zip/.tar.gz next to the source, or another app dropping a file) show up
// without a manual refresh.
void DesktopIconsWidget::ensure_desktop_watcher()
{
    if (m_desktop_watcher)
        return;
    auto watcher = Core::FileWatcher::create();
    if (watcher.is_error())
        return;
    m_desktop_watcher = watcher.release_value();

    auto directory = desktop_directory();
    auto mask = Core::FileWatcherEvent::Type::ChildCreated
        | Core::FileWatcherEvent::Type::ChildDeleted
        | Core::FileWatcherEvent::Type::ContentModified
        | Core::FileWatcherEvent::Type::MetadataModified;
    auto added = m_desktop_watcher->add_watch(directory, mask);
    if (added.is_error() || !added.value()) {
        m_desktop_watcher.clear();
        return;
    }
    m_desktop_watcher->on_change = [this](Core::FileWatcherEvent const&) {
        load_entries();
        update();
    };
}

void DesktopIconsWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);
    if (!m_loaded) {
        m_loaded = true;
        load_entries();
        ensure_desktop_watcher();
        ensure_wallpaper_watcher();
    }

    GUI::Painter painter(*this);

    // Solid desktop colour: also the whole background when "None" is selected,
    // and the fill behind Center/Tile wallpapers.
    Gfx::Color background_color = Gfx::Color::from_rgb(0x008080);
    if (!m_background_color.is_empty()) {
        if (auto parsed = Gfx::Color::from_string(m_background_color.view()); parsed.has_value())
            background_color = parsed.value();
    }
    painter.fill_rect(rect(), background_color);

    if (auto bitmap = wallpaper()) {
        auto source = bitmap->rect();
        if (m_wallpaper_mode == "Center"sv) {
            Gfx::IntRect target { 0, 0, source.width(), source.height() };
            target.center_within(rect());
            painter.draw_scaled_bitmap(target, *bitmap, source);
        } else if (m_wallpaper_mode == "Tile"sv) {
            painter.draw_tiled_bitmap(rect(), *bitmap);
        } else if (m_wallpaper_mode == "Stretch"sv) {
            painter.draw_scaled_bitmap(rect(), *bitmap, source);
        } else { // "Fill" (default): scale to cover, then centre-crop.
            auto scale = max(static_cast<float>(width()) / static_cast<float>(source.width()),
                static_cast<float>(height()) / static_cast<float>(source.height()));
            Gfx::IntRect target { 0, 0, static_cast<int>(static_cast<float>(source.width()) * scale),
                static_cast<int>(static_cast<float>(source.height()) * scale) };
            target.center_within(rect());
            painter.draw_scaled_bitmap(target, *bitmap, source);
        }
    }

    auto& font = Gfx::FontDatabase::default_font();
    auto label_font = font.with_size(10.0f);

    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto const& entry = m_entries[i];
        if (is_selected(static_cast<int>(i))) {
            painter.fill_rect(entry.rect, Color::from_argb(0x50000080));
            painter.draw_rect(entry.rect, Color::from_rgb(0xb0c8ff));
        }

        auto icon_rect = Gfx::IntRect { entry.rect.x() + (entry.rect.width() - ICON_SIZE) / 2, entry.rect.y() + 4, ICON_SIZE, ICON_SIZE };
        if (entry.icon)
            painter.draw_scaled_bitmap(icon_rect, *entry.icon, entry.icon->rect());
        else
            painter.fill_rect(icon_rect, Color::from_rgb(0x808080));

        auto text_top = icon_rect.bottom() + 4;
        auto text_rect = Gfx::IntRect { entry.rect.x(), text_top, entry.rect.width(), entry.rect.bottom() - text_top };
        painter.draw_text(text_rect.translated(1, 1), entry.label, *label_font, Gfx::TextAlignment::Center, Color::from_rgb(0x000000));
        painter.draw_text(text_rect, entry.label, *label_font, Gfx::TextAlignment::Center, Color::from_rgb(0xffffff));
    }

    // Rubber-band selection rectangle.
    if (m_marquee_active && !m_marquee_rect.is_empty()) {
        painter.fill_rect(m_marquee_rect, Color::from_argb(0x30000080));
        painter.draw_rect(m_marquee_rect, Color::from_rgb(0x000080));
    }
}

int DesktopIconsWidget::entry_at(Gfx::IntPoint position) const
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rect.contains(position))
            return static_cast<int>(i);
    }
    return -1;
}

bool DesktopIconsWidget::is_selected(int index) const
{
    for (auto i : m_selection) {
        if (i == index)
            return true;
    }
    return false;
}

void DesktopIconsWidget::clear_selection()
{
    m_selection.clear();
    m_selected = -1;
}

void DesktopIconsWidget::select_only(int index)
{
    m_selection.clear();
    if (index >= 0) {
        m_selection.append(index);
        m_selected = index;
    } else {
        m_selected = -1;
    }
}

void DesktopIconsWidget::mousedown_event(GUI::MouseEvent& event)
{
    m_might_drag = false;
    m_moving_icon = false;

    int const index = entry_at(event.position());
    bool const ctrl = event.ctrl();

    if (index >= 0 && event.button() == GUI::MouseButton::Primary) {
        if (ctrl) {
            // Ctrl+click toggles membership without starting a drag.
            if (is_selected(index)) {
                for (size_t k = 0; k < m_selection.size();) {
                    if (m_selection[k] == index)
                        m_selection.remove(k);
                    else
                        ++k;
                }
                if (m_selected == index)
                    m_selected = m_selection.is_empty() ? -1 : m_selection.last();
            } else {
                m_selection.append(index);
                m_selected = index;
            }
            update();
            return GUI::Widget::mousedown_event(event);
        }

        // Plain click: select only this icon and arm the icon-drag gesture.
        select_only(index);
        auto const& rect = m_entries[index].rect;
        m_press_position = event.position();
        m_grab_offset = event.position() - rect.location();
        m_press_rect = rect;
        m_might_drag = true;
    } else if (event.button() == GUI::MouseButton::Primary) {
        // Press on empty space: start a rubber-band selection (Ctrl keeps the
        // existing selection so you can add to it later).
        if (!ctrl)
            clear_selection();
        m_marquee_active = true;
        m_marquee_start = event.position();
        m_marquee_rect = {};
    }

    update();
    GUI::Widget::mousedown_event(event);
}

// Two gestures share the press:
//   - moving the pointer over the desktop rearranges the icon (Win9x style
//     free placement, position is remembered)
//   - leaving the desktop turns the press into a text/uri-list drag, which is
//     how icons are dropped onto the quick launch bar
void DesktopIconsWidget::mousemove_event(GUI::MouseEvent& event)
{
    if (m_marquee_active) {
        m_marquee_rect = Gfx::IntRect::from_two_points(m_marquee_start, event.position());
        m_selection.clear();
        m_selected = -1;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].rect.intersects(m_marquee_rect))
                m_selection.append(static_cast<int>(i));
        }
        update();
        return GUI::Widget::mousemove_event(event);
    }

    if (!m_might_drag)
        return GUI::Widget::mousemove_event(event);

    if (!(event.buttons() & GUI::MouseButton::Primary) || m_selected < 0 || static_cast<size_t>(m_selected) >= m_entries.size()) {
        m_might_drag = false;
        m_moving_icon = false;
        return GUI::Widget::mousemove_event(event);
    }

    auto diff = event.position() - m_press_position;
    auto distance_travelled_squared = diff.x() * diff.x() + diff.y() * diff.y();
    constexpr int drag_distance_threshold = 5;
    if (!m_moving_icon && distance_travelled_squared <= drag_distance_threshold)
        return GUI::Widget::mousemove_event(event);

    if (!rect().contains(event.position())) {
        // Pointer left the desktop: put the icon back where it was and hand the
        // file to whoever is under the cursor.
        m_might_drag = false;
        m_moving_icon = false;
        m_entries[m_selected].rect = m_press_rect;
        update();
        start_drag_for_entry(m_entries[m_selected]);
        return;
    }

    m_moving_icon = true;
    auto new_rect = m_press_rect;
    new_rect.set_location(event.position() - m_grab_offset);
    new_rect.set_x(clamp(new_rect.x(), 0, max(0, width() - new_rect.width())));
    new_rect.set_y(clamp(new_rect.y(), 0, max(0, height() - new_rect.height())));
    m_entries[m_selected].rect = new_rect;
    update();

    GUI::Widget::mousemove_event(event);
}

void DesktopIconsWidget::start_drag_for_entry(Entry const& entry)
{
    // An event can sneak in between building the drag operation and its nested
    // event loop; ignore those instead of starting a second drag.
    if (m_is_dragging)
        return;

    TemporaryChange dragging { m_is_dragging, true };

    if (entry.source_file.is_empty())
        return;

    dbgln("desktop: starting drag for {}", entry.source_file);

    Vector<URL::URL> urls;
    urls.append(URL::create_with_file_scheme(entry.source_file));

    auto mime_data = Core::MimeData::construct();
    if (auto result = mime_data->set_urls(urls); result.is_error()) {
        warnln("shell: cannot build uri-list for {}: {}", entry.source_file, result.error());
        return;
    }

    auto drag_operation = GUI::DragOperation::construct();
    drag_operation->set_mime_data(mime_data);
    drag_operation->exec();
}

void DesktopIconsWidget::mouseup_event(GUI::MouseEvent& event)
{
    if (m_marquee_active) {
        m_marquee_active = false;
        m_marquee_rect = {};
        update();
        GUI::Widget::mouseup_event(event);
        return;
    }

    if (m_moving_icon && m_selected >= 0 && static_cast<size_t>(m_selected) < m_entries.size()) {
        auto const& entry = m_entries[m_selected];
        save_position(LexicalPath::basename(entry.source_file), entry.rect.location());
    }
    m_might_drag = false;
    m_moving_icon = false;
    GUI::Widget::mouseup_event(event);
}

void DesktopIconsWidget::drag_enter_event(GUI::DragEvent& event)
{
    if (event.mime_data().has_urls())
        event.accept();
}

// Files dropped anywhere on the desktop are moved into ~/Desktop (copied when
// the source is on another filesystem) and the new icon lands where the pointer
// was released.
void DesktopIconsWidget::drop_event(GUI::DropEvent& event)
{
    event.accept();

    if (!event.mime_data().has_urls())
        return;

    auto directory = desktop_directory();
    auto next_position = event.position();
    bool changed = false;

    for (auto& url : event.mime_data().urls()) {
        auto path = URL::percent_decode(url.serialize_path());
        if (path.is_empty() || !FileSystem::exists(path))
            continue;

        if (is_protected_path(path)) {
            dbgln("desktop: refusing to move system path {}", path);
            continue;
        }

        if (LexicalPath::dirname(path) == directory) {
            // Already on the desktop - just move the icon.
            save_position(LexicalPath::basename(path), next_position);
            next_position.translate_by(0, CELL_HEIGHT);
            changed = true;
            continue;
        }

        auto name = unique_desktop_name(LexicalPath { path }.basename());
        auto target = ByteString::formatted("{}/{}", directory, name);

        if (auto result = FileSystem::move_file(target, path); result.is_error()) {
            warnln("shell: cannot move {} to {}: {}", path, target, result.error());
            continue;
        }

        dbgln("desktop: added {} (from {})", name, path);
        save_position(name, next_position);
        next_position.translate_by(0, CELL_HEIGHT);
        changed = true;
    }

    if (changed) {
        load_entries();
        update();
    }
}

// Launch an entry. Windows binaries go through the compatibility launcher
// (<prefix>/bin/serenity-winapp): "Open" (force_game = false) uses the app
// path (upstream wine, DWProton fallback), while the game-mode entry
// (force_game = true) forces umu + DWProton (per-game protonfixes).
void DesktopIconsWidget::launch_entry(Entry const& entry, bool force_game)
{
    auto target = file_target(entry);

    if (!target.is_empty() && is_windows_binary(target)) {
        // One launcher for the desktop and the file manager. Auto lets
        // serenity-winapp consult winapps.ini (managed program -> own prefix
        // and mode); the explicit game entry forces umu-run + DWProton.
        ExplorerShared::launch_windows_binary(target,
            force_game ? ExplorerShared::WindowsRunMode::Game
                       : ExplorerShared::WindowsRunMode::Auto);
        return;
    }

    // Plain file or folder that lives in ~/Desktop: there is no executable to
    // spawn, so let LaunchServer pick the handler.
    if (entry.exec.is_empty()) {
        if (target.is_empty())
            return;
        if (!Desktop::Launcher::open(URL::create_with_url_or_path(target)))
            show_error(ByteString::formatted("{}: {}", target, "failed to open"sv));
        return;
    }

    auto executable = PortHostify::resolve_executable(entry.exec);
    Vector<char const*> no_arguments;
    if (auto result = Core::Process::spawn(executable.view(),
            ReadonlySpan<char const*> { no_arguments.data(), no_arguments.size() },
            Core::StandardPaths::home_directory());
        result.is_error())
        warnln("shell: failed to launch {}: {}", executable, result.error());
}

// "foo.txt" -> "foo (2).txt" when ~/Desktop already has a foo.txt.
ByteString DesktopIconsWidget::unique_desktop_name(StringView desired)
{
    auto directory = desktop_directory();
    auto exists = [&](StringView name) {
        return FileSystem::exists(ByteString::formatted("{}/{}", directory, name));
    };

    if (!exists(desired))
        return desired.to_byte_string();

    // LexicalPath must outlive the StringViews handed out by its getters.
    LexicalPath path { desired };
    auto stem = path.title();
    auto extension = path.extension();
    auto suffix = extension.is_empty() ? ByteString {} : ByteString::formatted(".{}", extension);

    for (int i = 2; i < 1000; ++i) {
        auto candidate = ByteString::formatted("{} ({}){}", stem, i, suffix);
        if (!exists(candidate))
            return candidate;
    }
    return ByteString::formatted("{}-{}", stem, getpid());
}

void DesktopIconsWidget::create_folder_on_desktop()
{
    auto name = unique_desktop_name(translated("NewFolderName"sv, "New Folder"sv));
    auto path = ByteString::formatted("{}/{}", desktop_directory(), name);
    // Directory::create(CreateDirectories::No) only opens an existing directory
    // ("FIXME: doesn't work on Linux probably"), so mkdir directly.
    if (auto result = Core::System::mkdir(path, 0755); result.is_error()) {
        show_error(ByteString::formatted("{}: {}", path, result.error()));
        return;
    }
    reveal_new_entry(name);
}

void DesktopIconsWidget::create_text_file_on_desktop()
{
    auto name = unique_desktop_name(translated("NewTextFileName"sv, "New Text Document.txt"sv));
    auto path = ByteString::formatted("{}/{}", desktop_directory(), name);
    auto file = Core::File::open(path, Core::File::OpenMode::Write);
    if (file.is_error()) {
        show_error(ByteString::formatted("{}: {}", path, file.error()));
        return;
    }
    (void)file.value()->close();
    reveal_new_entry(name);
}

// "Clean memory cache": drop the kernel page cache, dentries and inodes.
// Only memory is touched - files under ~/.cache are never deleted, because
// some of them (Firefox profile cache, protonfixes, dconf) belong to programs.
ByteString DesktopIconsWidget::clear_memory_cache()
{
    auto available_before = available_memory_kb();
    if (!drop_memory_caches()) {
        return ByteString::formatted("{}",
            translated("CacheMemorySkipped"sv, "Needs root - skipped"sv));
    }

    auto available_after = available_memory_kb();
    auto freed_kb = available_after > available_before ? available_after - available_before : 0;
    dbgln("shell: dropped memory caches ({} -> {} kB available)", available_before, available_after);
    return ByteString::formatted("{}: {} ({} -> {})",
        translated("CacheMemoryFreed"sv, "Memory cache dropped"sv),
        human_readable_size(freed_kb * 1024),
        human_readable_size(available_before * 1024),
        human_readable_size(available_after * 1024));
}

// --- Desktop-local clipboard ----------------------------------------------

void DesktopIconsWidget::copy_entry_to_clipboard(int index, bool cut)
{
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size())
        return;
    copy_entries_to_clipboard({ index }, cut);
}

void DesktopIconsWidget::copy_entries_to_clipboard(Vector<int> const& indices, bool cut)
{
    m_clipboard_paths.clear();
    m_clipboard_is_cut = cut;

    // Publish everything on the system clipboard too, so pasting into a file
    // manager (or a terminal) works and the clipboard history records it.
    // text/uri-list is inherently multi-line, so a box-select + cut lands as a
    // multi-file clipboard.
    StringBuilder uri_list;
    for (int index : indices) {
        if (index < 0 || static_cast<size_t>(index) >= m_entries.size())
            continue;
        auto const& entry = m_entries[index];
        auto path = entry.path.is_empty() ? entry.source_file : entry.path;
        if (path.is_empty())
            continue;
        m_clipboard_paths.append(path);
        uri_list.append(URL::create_with_file_scheme(path).to_byte_string());
        uri_list.append('\n');
    }

    if (m_clipboard_paths.is_empty())
        return;

    HashMap<ByteString, ByteString> metadata;
    if (cut)
        metadata.set("x-serenity-cut"sv, "1"sv);
    GUI::Clipboard::the().set_data(uri_list.string_view().bytes(), "text/uri-list"sv, metadata);

    dbgln("desktop: clipboard <- {} items ({})", m_clipboard_paths.size(), cut ? "cut" : "copy");
}

void DesktopIconsWidget::paste_into_desktop()
{
    // The list of files to drop on the desktop. Prefer what this desktop
    // remembered (a cut/copy we performed); otherwise read the whole
    // text/uri-list the system clipboard currently holds.
    Vector<ByteString> sources;
    bool const cut = m_clipboard_is_cut;
    if (!m_clipboard_paths.is_empty()) {
        sources = m_clipboard_paths;
    } else {
        auto data_and_type = GUI::Clipboard::the().fetch_data_and_type();
        if (data_and_type.mime_type.is_empty() || !data_and_type.mime_type.contains("uri-list"sv))
            return;
        for (auto line : StringView { data_and_type.data }.split_view('\n')) {
            if (line.ends_with('\r'))
                line = line.substring_view(0, line.length() - 1);
            if (line.starts_with("file://"sv)) {
                auto path = URL::percent_decode(line.substring_view("file://"sv.length()));
                if (!path.is_empty())
                    sources.append(path);
            }
        }
    }

    if (sources.is_empty())
        return;

    auto directory = desktop_directory();
    auto next_position = m_last_menu_position;
    bool changed = false;

    for (auto const& source : sources) {
        if (source.is_empty() || !FileSystem::exists(source))
            continue;

        if (is_protected_path(source)) {
            dbgln("desktop: refusing to move system path {}", source);
            continue;
        }

        if (LexicalPath::dirname(source) == directory) {
            // Already on the desktop - just move the icon.
            save_position(LexicalPath::basename(source), next_position);
            next_position.translate_by(0, CELL_HEIGHT);
            changed = true;
            continue;
        }

        auto name = unique_desktop_name(LexicalPath { source }.basename());
        auto target = ByteString::formatted("{}/{}", directory, name);
        if (target == source)
            continue;

        auto result = cut
            ? FileSystem::move_file(target, source)
            : FileSystem::copy_file_or_directory(target, source,
                FileSystem::RecursionMode::Allowed, FileSystem::LinkMode::Allowed,
                FileSystem::AddDuplicateFileMarker::No);
        if (result.is_error()) {
            show_error(ByteString::formatted("{}: {}", source, result.error()));
            continue;
        }

        dbgln("desktop: added {} (from {})", name, source);
        save_position(name, next_position);
        next_position.translate_by(0, CELL_HEIGHT);
        changed = true;
    }

    // A cut consumed the clipboard.
    if (cut) {
        m_clipboard_paths.clear();
        m_clipboard_is_cut = false;
    }

    if (changed) {
        load_entries();
        update();
    }
}

// Show a freshly created file/folder where the context menu was opened.
void DesktopIconsWidget::reveal_new_entry(ByteString const& name)
{
    auto position = m_last_menu_position;
    position.set_x(clamp(position.x(), 0, max(0, width() - (CELL_WIDTH - 8))));
    position.set_y(clamp(position.y(), 0, max(0, height() - (CELL_HEIGHT - 6))));
    save_position(name, position);
    load_entries();
    update();
}

void DesktopIconsWidget::doubleclick_event(GUI::MouseEvent& event)
{
    if (auto index = entry_at(event.position()); index >= 0)
        launch_entry(m_entries[index]);
    GUI::Widget::doubleclick_event(event);
}

void DesktopIconsWidget::context_menu_event(GUI::ContextMenuEvent& event)
{
    // Same wording / order / icons as the file manager: both build their menus
    // from Ports/LinuxGUI/Common/ExplorerMenus.h. Only desktop-only commands
    // (arrange icons, drop memory caches, paste shortcut) are appended here.
    m_menu = GUI::Menu::construct();
    RefPtr<GUI::Action> default_action;
    m_last_menu_position = event.position();
    int const index = entry_at(event.position());

    auto open_settings = []() {
        Vector<char const*> no_arguments;
        if (auto result = Core::Process::spawn("serenity-settings"sv,
                ReadonlySpan<char const*> { no_arguments.data(), no_arguments.size() });
            result.is_error())
            warnln("shell: failed to launch serenity-settings: {}", result.error());
    };
    auto open_terminal_at = [](ByteString const& directory) {
        Vector<ByteString> no_arguments;
        auto executable = PortHostify::resolve_executable("serenity-terminal"sv);
        if (auto result = Core::Process::spawn(executable.view(),
                ReadonlySpan<ByteString> { no_arguments.data(), no_arguments.size() }, directory);
            result.is_error())
            warnln("shell: failed to launch terminal: {}", result.error());
    };
    auto clipboard_has_files = []() {
        return GUI::Clipboard::the().fetch_mime_type().contains("uri-list"sv);
    };
    auto add_desktop_only_actions = [&]() {
        m_menu->add_separator();
        auto arrange = m_menu->add_submenu(MUST(String::from_byte_string(translated("ArrangeIcons"sv, "&Arrange &Icons"sv))));
        arrange->add_action(GUI::Action::create(translated("LineUpIcons"sv, "&Line up Icons"sv), [this](auto&) {
            clear_positions();
            for (auto& entry : m_entries)
                entry.rect = {};
            layout_entries();
            update();
        }));
        m_menu->add_action(GUI::Action::create(translated("CleanMemoryCache"sv, "&Clean memory cache"sv), [this](auto&) {
            show_information(clear_memory_cache());
        }));
        m_menu->add_action(GUI::Action::create(translated("Refresh"sv, "&Refresh"sv), [this](auto&) {
            AppCatalog::the().rescan();
            load_entries();
            update();
        }));
        auto paste_shortcut = GUI::Action::create(translated("PasteShortcut"sv, "Paste &Shortcut"sv), [](auto&) {});
        paste_shortcut->set_enabled(false);
        m_menu->add_action(paste_shortcut);
    };

    if (index < 0) {
        auto background_actions = ExplorerShared::make_background_actions();
        background_actions->paste = [this]() { paste_into_desktop(); };
        background_actions->can_paste = !m_clipboard_paths.is_empty() || clipboard_has_files();
        background_actions->can_write = true;
        // The desktop's "New" submenu also offers file templates and shortcuts,
        // so let it fill the shared submenu instead of getting two of them.
        background_actions->suppress_default_new_items = true;
        background_actions->fill_new_menu = [this](GUI::Menu& menu) { add_new_menu(menu); };
        background_actions->open_in_terminal = [&]() { open_terminal_at(desktop_directory()); };
        background_actions->refresh = [this]() {
            AppCatalog::the().rescan();
            load_entries();
            update();
        };
        background_actions->properties = [open_settings]() { open_settings(); };
        ExplorerShared::append_background_actions(*m_menu, background_actions);
        add_desktop_only_actions();
    } else {
        // Right-clicking an icon that is not currently selected selects just it
        // (standard desktop behaviour); right-clicking one that is part of a
        // box-select keeps the whole group so the menu acts on all of them.
        if (!is_selected(index))
            select_only(index);

        auto const& entry = m_entries[index];
        ByteString target = file_target(entry);

        ExplorerShared::ItemContext item_context;
        for (int i : m_selection) {
            if (i < 0 || static_cast<size_t>(i) >= m_entries.size())
                continue;
            auto const& e = m_entries[i];
            ByteString t = file_target(e);
            if (e.is_shortcut) {
                item_context.entries.append(ExplorerShared::Entry::from_shortcut(
                    e.label.to_byte_string(), e.exec, t, e.source_file));
            } else {
                item_context.entries.append(ExplorerShared::Entry::from_path(t));
            }
        }
        item_context.can_paste = !m_clipboard_paths.is_empty() || clipboard_has_files();
        item_context.can_write = true;

        auto actions = ExplorerShared::make_item_actions();
        actions->open = [this]() {
            for (int i : m_selection) {
                if (i >= 0 && static_cast<size_t>(i) < m_entries.size())
                    launch_entry(m_entries[i]);
            }
        };
        actions->open_in_new_window = [target]() {
            if (!target.is_empty() && FileSystem::is_directory(target))
                Desktop::Launcher::open(URL::create_with_file_scheme(target));
        };
        actions->open_in_terminal = [open_terminal_at, target]() {
            if (!target.is_empty() && FileSystem::is_directory(target))
                open_terminal_at(target);
        };
        actions->run_with_wine = [target]() {
            ExplorerShared::launch_windows_binary(target, ExplorerShared::WindowsRunMode::App);
        };
        actions->run_as_game = [target]() {
            ExplorerShared::launch_windows_binary(target, ExplorerShared::WindowsRunMode::Game);
        };
        actions->fill_open_with = [target](GUI::Menu& menu) {
            if (target.is_empty())
                return;
            for (auto const& app : AppCatalog::the().apps()) {
                auto executable = PortHostify::resolve_executable(app->executable());
                menu.add_action(GUI::Action::create(app->name(), [executable, target](auto&) {
                    Vector<ByteString> arguments { target };
                    if (auto result = Core::Process::spawn(executable.view(),
                            ReadonlySpan<ByteString> { arguments.data(), arguments.size() },
                            LexicalPath::dirname(target));
                        result.is_error())
                        warnln("shell: open-with {} failed: {}", executable, result.error());
                }));
            }
        };
        actions->cut = [this]() { copy_entries_to_clipboard(m_selection, true); };
        actions->copy = [this]() { copy_entries_to_clipboard(m_selection, false); };
        actions->copy_path = [&]() {
            ByteString path = target.is_empty() ? entry.source_file : target;
            GUI::Clipboard::the().set_plain_text(path);
        };
        actions->paste = [this]() { paste_into_desktop(); };
        actions->remove = [this]() { delete_entries(m_selection); };
        actions->rename = [this, index]() { rename_entry_at(index); };
        actions->compress = [this, target](bool use_zip) { compress_target(target, use_zip); };
        actions->extract = [this, target]() { extract_target(target); };
        actions->set_wallpaper = [target]() {
            auto bitmap = Gfx::Bitmap::load_from_file(target, 1, GUI::Desktop::the().rect().size());
            if (bitmap.is_error())
                return;
            GUI::Desktop::the().set_wallpaper(bitmap.release_value(), target);
        };
        actions->properties = [open_settings]() { open_settings(); };

        default_action = ExplorerShared::append_item_actions(*m_menu, item_context, actions);
        add_desktop_only_actions();
    }

    m_menu->popup(event.screen_position(), default_action);
    GUI::Widget::context_menu_event(event);
}

void DesktopIconsWidget::remove_icon_at(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
        return;
    auto const& entry = m_entries[index];
    if (entry.source_file.is_empty() || !entry.is_shortcut)
        return;
    // Only remove user-owned shortcuts (those under the user's desktop dir);
    // system-provided ones under /usr/share/serenity/desktop are read-only.
    auto directory = desktop_directory();
    if (!entry.source_file.starts_with(directory))
        return;
    if (FileSystem::remove(entry.source_file, FileSystem::RecursionMode::Disallowed).is_error())
        warnln("shell: cannot remove {}", entry.source_file);
    load_entries();
    update();
}

// Pre-Win10 Explorer has a "Delete" item on every icon. A user shortcut is
// removed by deleting its .af/.desktop source; a real file/folder placed on the
// desktop is the user's data and is deleted from disk (after confirmation).
void DesktopIconsWidget::delete_entry_at(int index)
{
    delete_entries({ index });
}

// Delete a batch of icons. A user shortcut is removed by deleting its
// .af/.desktop source; a real file/folder placed on the desktop is the user's
// data and is deleted from disk (after a single confirmation for the group).
void DesktopIconsWidget::delete_entries(Vector<int> const& indices)
{
    // Dedupe, dropping out-of-range indices.
    Vector<int> unique;
    for (int i : indices) {
        if (i < 0 || static_cast<size_t>(i) >= m_entries.size())
            continue;
        bool dup = false;
        for (int u : unique) {
            if (u == i) {
                dup = true;
                break;
            }
        }
        if (!dup)
            unique.append(i);
    }
    if (unique.is_empty())
        return;

    struct ToDelete {
        ByteString path;
        ByteString label;
    };
    Vector<ToDelete> targets;
    for (int i : unique) {
        auto const& entry = m_entries[i];
        ByteString path;
        if (entry.is_shortcut) {
            auto directory = desktop_directory();
            if (entry.source_file.is_empty() || !entry.source_file.starts_with(directory))
                continue; // system-managed shortcut: not deletable here
            path = entry.source_file;
        } else {
            path = file_target(entry);
        }
        if (path.is_empty())
            continue;
        targets.append({ path, entry.label.to_byte_string() });
    }

    if (targets.is_empty()) {
        show_information("系统快捷方式由系统管理，无法删除。"sv);
        return;
    }

    ByteString display;
    if (targets.size() == 1) {
        display = targets[0].label.is_empty() ? ByteString { LexicalPath { targets[0].path }.basename() } : targets[0].label;
    } else {
        display = ByteString::formatted("{} 个项目", targets.size());
    }
    if (GUI::MessageBox::show(window(),
            ByteString::formatted("确实要删除“{}”吗？", display),
            "确认删除"sv, GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNo)
        != GUI::Dialog::ExecResult::Yes)
        return;

    for (auto& t : targets) {
        struct stat st;
        bool is_dir = ::stat(t.path.characters(), &st) == 0 && S_ISDIR(st.st_mode);
        auto recursion = is_dir ? FileSystem::RecursionMode::Allowed : FileSystem::RecursionMode::Disallowed;
        if (auto result = FileSystem::remove(t.path, recursion); result.is_error())
            show_error(ByteString::formatted("无法删除 {}：{}", t.path, result.error()));
    }

    clear_selection();
    load_entries();
    update();
}

// Rename a real file/folder on the desktop (shortcuts carry their display name
// inside the .desktop/.af file, so renaming the file would not change the
// label - skip them here rather than offer a confusing no-op).
void DesktopIconsWidget::rename_entry_at(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
        return;
    auto const& entry = m_entries[index];
    if (entry.is_shortcut)
        return;

    auto path = file_target(entry);
    if (path.is_empty())
        return;

    LexicalPath lp(path);
    ByteString current = entry.label.to_byte_string();
    if (current.is_empty())
        current = ByteString(lp.basename());

    String new_name = MUST(String::from_byte_string(current));
    if (GUI::InputBox::show(window(), new_name, "请输入新名称："sv, "重命名"sv) != GUI::Dialog::ExecResult::OK)
        return;
    auto trimmed = new_name.to_byte_string().trim_whitespace();
    if (trimmed.is_empty() || trimmed == current)
        return;

    auto new_path = ByteString::formatted("{}/{}", lp.dirname(), trimmed);
    if (FileSystem::exists(new_path)) {
        show_error(ByteString::formatted("“{}”已存在。", trimmed));
        return;
    }
    if (auto result = FileSystem::move_file(path, new_path); result.is_error()) {
        show_error(ByteString::formatted("无法重命名：{}", result.error()));
        return;
    }
    load_entries();
    update();
}

// "New > Shortcut > <app>": write a .desktop shortcut for an installed app.
void DesktopIconsWidget::add_icon_menu(GUI::Menu& menu)
{
    // Same catalog the start menu uses: one scan, one launchability filter.
    for (auto const& app : AppCatalog::the().apps()) {
        auto name = app->menu_name().is_empty() ? app->name() : app->menu_name();
        auto icon_path = app->icon_path();
        auto exec = app->executable();
        menu.add_action(GUI::Action::create(ByteString { name },
            [this, name, icon_path, exec](auto&) {
                write_shortcut_file(name, exec, icon_path);
                load_entries();
                update();
            }));
    }
}

// "New" submenu: a folder, then one entry per file template, then shortcuts.
// Templates are plain files, so adding "New > Spreadsheet" is a matter of
// dropping a file into <prefix>/share/templates or ~/Templates.
void DesktopIconsWidget::add_new_menu(GUI::Menu& menu)
{
    menu.add_action(GUI::Action::create(translated("NewFolder"sv, "&Folder"sv), [this](auto&) {
        create_folder_on_desktop();
    }));

    auto templates = template_paths();
    if (templates.is_empty()) {
        // Nothing installed: keep "Text Document" working anyway.
        menu.add_action(GUI::Action::create(translated("NewTextFile"sv, "&Text Document"sv), [this](auto&) {
            create_text_file_on_desktop();
        }));
    } else {
        for (auto const& path : templates) {
            // Keep the LexicalPath alive: basename()/title()/extension() return
            // StringViews into it, and a temporary would leave them dangling.
            LexicalPath template_path { path };
            auto stem = template_path.title();
            auto extension = template_path.extension();
            // "Text Document" -> 文本文档 in the localized UI, and the new file
            // is created with the translated name.
            auto label = PortTranslation::tr("Templates"sv, stem, stem);
            menu.add_action(GUI::Action::create(ByteString { label },
                [this, path, label, extension = ByteString { extension }](auto&) {
                    create_file_from_template(path, label, extension);
                }));
        }
    }

    menu.add_separator();
    auto shortcuts = menu.add_submenu(MUST(String::from_byte_string(translated("NewShortcut"sv, "&Shortcut"sv))));
    add_icon_menu(*shortcuts);
}

Vector<ByteString> DesktopIconsWidget::template_paths()
{
    Vector<ByteString> paths;
    HashMap<ByteString, size_t> seen;

    auto scan = [&](ByteString const& directory) {
        Core::DirIterator it(directory, Core::DirIterator::SkipDots);
        if (it.has_error())
            return;
        while (it.has_next()) {
            auto name = it.next_path();
            if (name.starts_with('.'))
                continue;
            auto path = ByteString::formatted("{}/{}", directory, name);
            if (!FileSystem::is_regular_file(path))
                continue;
            if (auto found = seen.find(name); found != seen.end()) {
                // User template overrides the system one with the same name.
                paths[found->value] = path;
                continue;
            }
            seen.set(name, paths.size());
            paths.append(path);
        }
    };

    scan(system_templates_directory());
    scan(user_templates_directory());

    quick_sort(paths, [](auto const& a, auto const& b) { return a < b; });
    return paths;
}

void DesktopIconsWidget::create_file_from_template(ByteString const& template_path, StringView display_name, StringView extension)
{
    // extension() comes back without the dot, and a template without one is
    // created exactly as named.
    auto desired = extension.is_empty()
        ? ByteString { display_name }
        : ByteString::formatted("{}.{}", display_name, extension);
    auto name = unique_desktop_name(desired);
    auto target = ByteString::formatted("{}/{}", desktop_directory(), name);

    if (auto result = FileSystem::copy_file_or_directory(target, template_path,
            FileSystem::RecursionMode::Disallowed, FileSystem::LinkMode::Disallowed,
            FileSystem::AddDuplicateFileMarker::No);
        result.is_error()) {
        show_error(ByteString::formatted("{}: {}", target, result.error()));
        return;
    }

    reveal_new_entry(name);
}

// File target for compression: an explicit Path=, or an Exec that is an
// absolute path to an existing file/folder.
ByteString DesktopIconsWidget::file_target(Entry const& entry) const
{
    struct stat st;
    if (!entry.path.is_empty() && stat(entry.path.characters(), &st) == 0)
        return entry.path;
    if (entry.exec.starts_with("/"sv) && stat(entry.exec.characters(), &st) == 0)
        return entry.exec;
    return {};
}

bool DesktopIconsWidget::is_archive(ByteString const& path)
{
    return PortArchiveOps::is_archive(path);
}

void DesktopIconsWidget::show_information(StringView text)
{
    GUI::MessageBox::show(nullptr, text, PortTranslation::tr("Desktop"sv, "MessageBoxTitle"sv, "Desktop"sv), GUI::MessageBox::Type::Information);
}

void DesktopIconsWidget::show_error(StringView text)
{
    GUI::MessageBox::show(nullptr, text, PortTranslation::tr("Desktop"sv, "MessageBoxTitle"sv, "Desktop"sv), GUI::MessageBox::Type::Error);
}

// Run `tool args...` in `workdir`; report failures via MessageBox.
void DesktopIconsWidget::run_tool(ByteString const& tool, Vector<ByteString> const& arguments, ByteString const& workdir)
{
    auto result = Core::command(tool, arguments, LexicalPath { workdir });
    if (result.is_error()) {
        show_error(ByteString::formatted("{}: {}", tool, result.error()));
        return;
    }
    if (result.value().exit_code != 0) {
        show_error(ByteString::formatted("{} exited with {}\n\n{}",
            tool, result.value().exit_code,
            String::from_utf8_with_replacement_character(result.value().error)));
        return;
    }
    show_information(PortTranslation::tr("Desktop"sv, "OperationDone"sv, "Done."sv));
}

void DesktopIconsWidget::compress_target(ByteString const& source, bool use_zip)
{
    if (auto error = PortArchiveOps::compress(source, use_zip); !error.is_empty())
        show_error(error);
    else
        show_information(PortTranslation::tr("Desktop"sv, "OperationDone"sv, "Done."sv));
}

void DesktopIconsWidget::extract_target(ByteString const& source)
{
    if (auto error = PortArchiveOps::extract(source); !error.is_empty())
        show_error(error);
    else
        show_information(PortTranslation::tr("Desktop"sv, "OperationDone"sv, "Done."sv));
}
