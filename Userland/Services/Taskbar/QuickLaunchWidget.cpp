/*
 * Copyright (c) 2021, Fabian Blatz <fabianblatz@gmail.com>
 * Copyright (c) 2023, David Ganz <david.g.ganz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "QuickLaunchWidget.h"
#include <AK/LexicalPath.h>
#include <Kernel/API/InodeWatcherFlags.h>
#include <LibConfig/Client.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Directory.h>
#include <LibCore/MimeData.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/FileIconProvider.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <sys/stat.h>

namespace Taskbar {

static ByteString sanitize_name(ByteString const& name)
{
    return name.replace(" "sv, ""sv).replace("="sv, ""sv);
}

static ByteString entry_to_config_string(size_t index, NonnullOwnPtr<QuickLaunchEntry> const& entry)
{
    return ByteString::formatted("{}:{}", index, entry->path());
}

OwnPtr<QuickLaunchEntry> QuickLaunchEntry::create_from_path(StringView path)
{
    if (path.ends_with(".af"sv)) {
        auto af_path = path.to_byte_string();
        if (!path.starts_with('/'))
            af_path = ByteString::formatted("{}/{}", Desktop::AppFile::APP_FILES_DIRECTORY, path);

        auto app_file = Desktop::AppFile::open(af_path);
        // A dangling quick-launch symlink (e.g. FirefoxESR.af pointing at a
        // removed /res/apps entry) yields an invalid AppFile with an empty
        // Name. QuickLaunchEntryAppFile::name() then trips
        // VERIFY(!name.is_empty()) and takes the whole shell down. Skip bad
        // entries instead of crashing the session.
        if (!app_file->is_valid())
            return {};
        return make<QuickLaunchEntryAppFile>(app_file);
    }

    if (path.ends_with(".desktop"sv))
        return make<QuickLaunchEntryDesktopFile>(path.to_byte_string());

    auto stat_or_error = Core::System::stat(path);
    if (stat_or_error.is_error()) {
        dbgln("Failed to stat quick launch entry file: {}", stat_or_error.release_error());
        return {};
    }

    auto stat = stat_or_error.release_value();
    if (S_ISREG(stat.st_mode) && ((stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0))
        return make<QuickLaunchEntryExecutable>(path);
    return make<QuickLaunchEntryFile>(path);
}

ErrorOr<void> QuickLaunchEntryAppFile::launch() const
{
    TRY(m_app_file->spawn_with_escalation());
    return {};
}

ErrorOr<void> QuickLaunchEntryExecutable::launch() const
{
    TRY(Core::Process::spawn(m_path));
    return {};
}

GUI::Icon QuickLaunchEntryExecutable::icon() const
{
    return GUI::FileIconProvider::icon_for_executable(m_path);
}

ByteString QuickLaunchEntryExecutable::name() const
{
    return LexicalPath { m_path }.basename();
}

ErrorOr<void> QuickLaunchEntryFile::launch() const
{
    if (!Desktop::Launcher::open(URL::create_with_url_or_path(m_path))) {
        // FIXME: LaunchServer doesn't inform us about errors
        return Error::from_string_literal("Failed to open file");
    }
    return {};
}

GUI::Icon QuickLaunchEntryFile::icon() const
{
    return GUI::FileIconProvider::icon_for_path(m_path);
}
GUI::Icon QuickLaunchEntryAppFile::icon() const
{
    if (!m_cached_icon.has_value())
        m_cached_icon = m_app_file->icon();
    return *m_cached_icon;
}

QuickLaunchEntryDesktopFile::QuickLaunchEntryDesktopFile(ByteString path)
    : m_path(move(path))
{
    m_name = LexicalPath { m_path }.basename();

    auto config = Core::ConfigFile::open(m_path);
    if (config.is_error())
        return;

    auto const& file = *config.value();
    auto name = file.read_entry("Desktop Entry", "Name").trim_whitespace();
    if (!name.is_empty())
        m_name = name;

    m_icon_path = file.read_entry("Desktop Entry", "Icon").trim_whitespace();

    auto exec = file.read_entry("Desktop Entry", "Exec").trim_whitespace();
    // Drop the field codes ("%U", "%f", ...) that launchers append to Exec.
    if (auto percent = exec.find('%'); percent.has_value())
        exec = exec.substring(0, percent.value()).trim_whitespace();
    for (auto& argument : exec.split(' ')) {
        if (!argument.is_empty())
            m_arguments.append(argument);
    }
}

ErrorOr<void> QuickLaunchEntryDesktopFile::launch() const
{
    if (m_arguments.is_empty())
        return Error::from_string_literal("Desktop entry has no Exec line");

    Vector<StringView> arguments;
    for (size_t i = 1; i < m_arguments.size(); ++i)
        arguments.append(m_arguments[i].view());
    TRY(Core::Process::spawn(m_arguments[0], arguments));
    return {};
}

GUI::Icon QuickLaunchEntryDesktopFile::icon() const
{
    if (m_cached_icon.has_value())
        return *m_cached_icon;

    GUI::Icon icon;
    if (!m_icon_path.is_empty()) {
        if (m_icon_path.starts_with('/')) {
            if (auto bitmap_or_error = Gfx::Bitmap::load_from_file(m_icon_path); !bitmap_or_error.is_error()) {
                RefPtr<Gfx::Bitmap const> bitmap = bitmap_or_error.release_value();
                icon = GUI::Icon { move(bitmap) };
            }
        } else if (auto themed_icon = GUI::Icon::try_create_default_icon(m_icon_path); !themed_icon.is_error()) {
            // Freedesktop icon names ("Icon=calculator") are not paths.
            icon = themed_icon.release_value();
        }
    }
    m_cached_icon = icon;
    return icon;
}

ErrorOr<NonnullRefPtr<QuickLaunchWidget>> QuickLaunchWidget::create()
{
    auto widget = TRY(AK::adopt_nonnull_ref_or_enomem(new (nothrow) QuickLaunchWidget()));
    TRY(widget->create_context_menu());
    // NOTE: Loading must not persist. Writing the whole group here makes the
    // Config listener call load_entries() again while the write is still in
    // flight, so the reload reads a half-written config and the entry list
    // shrinks (6 -> 5 -> 4) every time the shell starts. The config is only
    // written when the user actually adds, removes or reorders an entry.
    widget->load_entries(false);
    return widget;
}

ErrorOr<bool> QuickLaunchWidget::add_from_pid(pid_t pid_to_add)
{
    auto processes_file = TRY(Core::File::open("/sys/kernel/processes"sv, Core::File::OpenMode::Read));
    auto file_content = TRY(processes_file->read_until_eof());
    auto json_obj = TRY(JsonValue::from_string(file_content)).as_object();
    for (auto value : json_obj.get_array("processes"sv).release_value().values()) {
        auto& process_object = value.as_object();
        auto pid = process_object.get_i32("pid"sv).value_or(0);
        if (pid != pid_to_add)
            continue;

        auto executable = process_object.get_byte_string("executable"sv);
        if (!executable.has_value())
            break;

        auto maybe_name = process_object.get_byte_string("name"sv);
        if (!maybe_name.has_value())
            break;

        auto name = maybe_name.release_value();
        auto path = executable.release_value();
        if (Desktop::AppFile::exists_for_app(name)) {
            path = Desktop::AppFile::app_file_path_for_app(name);
        }

        auto new_entry = QuickLaunchEntry::create_from_path(path);
        if (!new_entry)
            break;

        TRY(update_entry(name, new_entry.release_nonnull()));
        return true;
    }

    return false;
}

void QuickLaunchWidget::config_key_was_removed(StringView domain, StringView group, StringView key)
{
    if (domain == "Taskbar" && group == CONFIG_GROUP_ENTRIES)
        remove_entry(key, false);
}

void QuickLaunchWidget::config_string_did_change(StringView domain, StringView group, StringView, StringView)
{
    if (domain == "Taskbar" && group == CONFIG_GROUP_ENTRIES)
        load_entries(false);
}

void QuickLaunchWidget::drag_enter_event(GUI::DragEvent& event)
{
    if (event.mime_data().has_urls())
        event.accept();
}

void QuickLaunchWidget::drop_event(GUI::DropEvent& event)
{
    event.accept();

    if (event.mime_data().has_urls()) {
        auto urls = event.mime_data().urls();
        dbgln("quick-launch: drop with {} url(s)", urls.size());
        for (auto& url : urls) {
            auto path = URL::percent_decode(url.serialize_path());
            auto entry = QuickLaunchEntry::create_from_path(path);
            if (entry) {
                auto entry_name = entry->name();
                auto result = update_entry(entry_name, entry.release_nonnull());
                if (result.is_error())
                    GUI::MessageBox::show_error(window(), ByteString::formatted("Failed to add quick launch entry: {}", result.release_error()));
            }
        }
    }
}

void QuickLaunchWidget::mousedown_event(GUI::MouseEvent& event)
{
    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect rect) {
        if (m_dragging && !entry->is_pressed())
            return;

        entry->set_pressed(rect.contains(event.position()));
        if (entry->is_pressed())
            m_grab_offset = rect.x() - event.x();
    });
    update();
}

void QuickLaunchWidget::mousemove_event(GUI::MouseEvent& event)
{
    m_mouse_pos = event.position();
    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect rect) {
        entry->set_hovered(rect.contains(event.position()));

        int drag_distance = AK::abs(event.x() - (rect.x() - m_grab_offset));
        if (entry->is_pressed() && drag_distance > MINIMUM_DRAG_DISTANCE)
            m_dragging = true;

        // 悬停提示暂时关掉：这个移植里 Application::show_tooltip() 一调用就崩
        // （TooltipWindow 还没完全适配），先保证任务栏不挂。
    });

    if (m_dragging)
        recalculate_order();

    update();
}

void QuickLaunchWidget::mouseup_event(GUI::MouseEvent& event)
{
    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect) {
        if (!m_dragging && entry->is_pressed() && event.button() == GUI::MouseButton::Primary) {
            auto result = entry->launch();
            if (result.is_error()) {
                // FIXME: This message box is displayed in a weird position
                GUI::MessageBox::show_error(window(), ByteString::formatted("Failed to open quick launch entry: {}", result.release_error()));
            }
        }

        entry->set_pressed(false);
    });

    m_dragging = false;

    update();
}

void QuickLaunchWidget::context_menu_event(GUI::ContextMenuEvent& event)
{
    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect rect) {
        if (!rect.contains(event.position()))
            return;

        m_context_menu_app_name = entry->name();
        m_context_menu->popup(event.screen_position(), m_context_menu_default_action);
    });
}

void QuickLaunchWidget::leave_event(Core::Event& event)
{
    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, auto) {
        entry->set_pressed(false);
        entry->set_hovered(false);
    });

    m_dragging = false;
    m_grab_offset = 0;

    update();
    event.accept();
    Widget::leave_event(event);
}

void QuickLaunchWidget::resize_event(GUI::ResizeEvent& event)
{
    GUI::Frame::resize_event(event);
    update();
}

void QuickLaunchWidget::show_event(GUI::ShowEvent& event)
{
    GUI::Frame::show_event(event);
    update();
}

void QuickLaunchWidget::paint_event(GUI::PaintEvent& event)
{
    Frame::paint_event(event);

    GUI::Painter painter(*this);

    auto paint_entry = [this, &painter](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect rect) {
        if (entry->is_pressed() || entry->is_hovered())
            Gfx::StylePainter::paint_button(painter, rect, palette(), Gfx::ButtonStyle::Coolbar, entry->is_pressed(), entry->is_hovered());

        // Keep the Icon in a local. entry->icon() returns a temporary, and its
        // IconImpl is the only owner of the bitmaps, so writing
        // `entry->icon().bitmap_for_size(16)` left us with a pointer into freed
        // memory by the time we drew it - every button painted blank.
        auto icon = entry->icon();
        // Prefer the larger source and scale it down: Vista's quick-launch
        // icons are ~2/3 of the bar height, not the raw 16px asset.
        auto const* bitmap = icon.bitmap_for_size(32);
        if (!bitmap)
            bitmap = icon.bitmap_for_size(64);
        if (!bitmap)
            bitmap = icon.bitmap_for_size(16);
        if (!bitmap) {
            // The app has no app-*.png in /res/icons. Fall back to the generic
            // "unknown" icon so the button still reads as a button instead of a
            // dark hole in the taskbar.
            static auto fallback_icon = [] {
                auto result = GUI::Icon::try_create_default_icon("filetype-unknown"sv);
                if (result.is_error())
                    return GUI::Icon {};
                return result.release_value();
            }();
            bitmap = fallback_icon.bitmap_for_size(16);
            if (!bitmap)
                bitmap = fallback_icon.bitmap_for_size(32);
        }

        auto content_rect = rect.shrunken(2, 2);
        if (!bitmap) {
            painter.fill_rect(content_rect, palette().button().darkened(0.2f));
            return;
        }

        // The icon may be a HiDPI (2x) bitmap: blit() would VERIFY("painter
        // doesn't support downsampling scale factors") and kill the shell, so
        // measure in logical pixels and let draw_scaled_bitmap do the scaling.
        auto icon_side = min(22, min(content_rect.width(), content_rect.height()));
        Gfx::IntRect target { 0, 0, icon_side, icon_side };
        target.center_within(content_rect);
        if (entry->is_pressed())
            target.translate_by(1, 1);

        painter.draw_scaled_bitmap(target, *bitmap, bitmap->rect());
    };

    NonnullOwnPtr<QuickLaunchEntry> const* dragged_entry = nullptr;
    Gfx::IntRect dragged_entry_rect;

    for_each_entry([&](NonnullOwnPtr<QuickLaunchEntry> const& entry, Gfx::IntRect rect) {
        if (m_dragging && entry->is_pressed()) {
            rect.set_x(m_mouse_pos.x() + m_grab_offset);
            dragged_entry = &entry;
            dragged_entry_rect = rect;
            return;
        }

        paint_entry(entry, rect);
    });

    if (dragged_entry)
        paint_entry(*dragged_entry, dragged_entry_rect);
}

QuickLaunchWidget::QuickLaunchWidget()
{
    set_shrink_to_fit(true);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
    set_frame_style(Gfx::FrameStyle::NoFrame);
    set_fixed_height(21);
}

ErrorOr<void> QuickLaunchWidget::create_context_menu()
{
    auto icon = TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/delete.png"sv));
    m_context_menu = GUI::Menu::construct();
    m_context_menu_default_action = GUI::Action::create("&Remove", icon, [this](auto&) {
        remove_entry(m_context_menu_app_name);
        repaint();
    });
    m_context_menu->add_action(*m_context_menu_default_action);

    return {};
}


// 快速启动栏目录：<user data>/quick-launch。用户往里放 .af / 可执行文件即可。
static ByteString quick_launch_directory()
{
    auto data = Core::StandardPaths::data_directory();
    if (data.is_empty())
        data = ByteString::formatted("{}/.local/share", Core::StandardPaths::home_directory());
    return ByteString::formatted("{}/quick-launch", data);
}

static void ensure_default_quick_launch_entries(ByteString const& dir)
{
    if (Core::System::stat(dir).is_error()) {
        (void)Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes);
        // 只放这个构建里确实存在的 app。
        for (auto name : { "FileManager.af"sv, "Terminal.af"sv, "Settings.af"sv, "Calculator.af"sv }) {
            auto target = ByteString::formatted("/res/apps/{}", name);
            auto link = ByteString::formatted("{}/{}", dir, name);
            if (Core::System::stat(target).is_error())
                continue;
            (void)Core::System::symlink(target, link);
        }
    }
}

static Vector<ByteString> scan_quick_launch_directory(ByteString const& dir)
{
    Vector<ByteString> paths;
    (void)Core::Directory::for_each_entry(dir, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        paths.append(ByteString::formatted("{}/{}", dir, entry.name));
        return IterationDecision::Continue;
    });
    quick_sort(paths, [](auto const& a, auto const& b) { return a < b; });
    return paths;
}

void QuickLaunchWidget::load_entries(bool save)
{
    struct ConfigEntry {
        int index;
        ByteString path;
    };

    Vector<ConfigEntry> config_entries;
    auto keys = Config::list_keys(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES);
    if (!keys.is_empty()) {
        for (auto& name : keys) {
            auto value = Config::read_string(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, name);
            auto values = value.split(':');
            config_entries.append({ values[0].to_number<int>().release_value(), values[1] });
        }
    } else {
        // 快速启动栏的内容来自目录：<user data>/quick-launch/。
        // 首次运行自动建目录并放几个默认 .af 链接，之后用户往目录里丢/删文件即可，
        // 不再写死应用列表。
    }

    quick_sort(config_entries, [](ConfigEntry const& a, ConfigEntry const& b) {
        return a.index < b.index;
    });

    Vector<NonnullOwnPtr<QuickLaunchEntry>> entries;
    for (auto const& config_entry : config_entries) {
        auto entry = QuickLaunchEntry::create_from_path(config_entry.path);
        if (!entry)
            continue;

        entries.append(entry.release_nonnull());
    }

    auto dir = quick_launch_directory();
    ensure_default_quick_launch_entries(dir);
    for (auto const& path : scan_quick_launch_directory(dir)) {
        auto entry = QuickLaunchEntry::create_from_path(path);
        if (!entry)
            continue;
        auto name = entry->name();
        bool duplicate = false;
        for (auto const& existing : entries) {
            if (existing->name() == name) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            entries.append(entry.release_nonnull());
    }

    // backwards compatibility since the group and value-format changed
    auto old_keys = Config::list_keys(CONFIG_DOMAIN, OLD_CONFIG_GROUP_ENTRIES);
    if (!old_keys.is_empty()) {
        for (auto& name : old_keys) {
            auto path = Config::read_string(CONFIG_DOMAIN, OLD_CONFIG_GROUP_ENTRIES, name);
            auto entry = QuickLaunchEntry::create_from_path(path);
            if (!entry)
                continue;

            entries.append(entry.release_nonnull());
        }

        Config::remove_group(CONFIG_DOMAIN, OLD_CONFIG_GROUP_ENTRIES);
    }

    m_entries.clear();
    add_entries(move(entries), save);
}

void QuickLaunchWidget::add_entries(Vector<NonnullOwnPtr<QuickLaunchEntry>> entries, bool save)
{
    size_t size = entries.size();
    for (size_t i = 0; i < size; i++) {
        m_entries.append(entries.take(0));
        if (save)
            Config::write_string(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, sanitize_name(m_entries.last()->name()), entry_to_config_string(m_entries.size() - 1, m_entries.last()));
    }

    repaint();
}

ErrorOr<void> QuickLaunchWidget::update_entry(ByteString const& button_name, NonnullOwnPtr<QuickLaunchEntry> entry, bool save)
{
    auto file_name_to_watch = entry->file_name_to_watch();
    if (!file_name_to_watch.is_empty()) {
        if (!m_watcher) {
            m_watcher = TRY(Core::FileWatcher::create());
            m_watcher->on_change = [button_name, save, this](Core::FileWatcherEvent const&) {
                dbgln("Removing QuickLaunch entry \"{}\"", button_name);
                remove_entry(button_name, save);
                repaint();
            };
        }
        TRY(m_watcher->add_watch(file_name_to_watch, Core::FileWatcherEvent::Type::Deleted));
    }

    set_or_insert_entry(move(entry), save);
    repaint();

    return {};
}

template<typename Callback>
void QuickLaunchWidget::for_each_entry(Callback callback)
{
    // Buttons span the full height of the widget. The taskbar is 27px tall with
    // 3px margins, so this widget is 21px high: a hardcoded BUTTON_SIZE square
    // would paint three pixels below the clip and look cut off.
    Gfx::IntRect rect(0, 0, BUTTON_SIZE, height() > 0 ? height() : BUTTON_SIZE);
    for (auto const& entry : m_entries) {
        callback(entry, rect);
        rect.translate_by(BUTTON_SIZE, 0);
    }
}

void QuickLaunchWidget::resize()
{
    set_fixed_width(static_cast<int>(m_entries.size()) * BUTTON_SIZE);
    dbgln("quick-launch: {} entries, width {}", m_entries.size(), static_cast<int>(m_entries.size()) * BUTTON_SIZE);
}

void QuickLaunchWidget::repaint()
{
    resize();
    update();
}

void QuickLaunchWidget::set_or_insert_entry(NonnullOwnPtr<QuickLaunchEntry> entry, bool save)
{
    auto name = entry->name();
    for (size_t i = 0; i < m_entries.size(); i++) {
        auto& value = m_entries[i];
        if (value->name() != name)
            continue;
        value = move(entry);
        if (save)
            Config::write_string(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, sanitize_name(value->name()), entry_to_config_string(i, value));
        return;
    }

    if (save)
        Config::write_string(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, sanitize_name(entry->name()), entry_to_config_string(m_entries.size(), entry));
    m_entries.append(move(entry));
}

void QuickLaunchWidget::remove_entry(ByteString const& name, bool save)
{
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i]->name() != name)
            continue;
        if (save)
            Config::remove_key(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, m_entries[i]->name());
        m_entries.remove(i);
        return;
    }
}
void QuickLaunchWidget::recalculate_order()
{
    if (!m_dragging)
        return;

    size_t dragged_index = 0;
    for (; dragged_index < m_entries.size(); dragged_index++) {
        if (m_entries[dragged_index]->is_pressed())
            break;
    }

    size_t new_index = m_entries.size() + 1;
    Gfx::IntRect rect(0, 0, BUTTON_SIZE, BUTTON_SIZE);
    for (size_t i = 0; i < m_entries.size(); i++) {
        auto left_break_point = i == 0 ? rect.x() + rect.width() / 2 : rect.x();
        if (m_mouse_pos.x() < left_break_point) {
            new_index = i;
            break;
        }

        if (i == m_entries.size() - 1 && m_mouse_pos.x() > rect.x() + rect.width() / 2) {
            new_index = i + 1;
            break;
        }

        rect.translate_by(BUTTON_SIZE, 0);
    }

    if (new_index >= m_entries.size() + 1 || new_index == dragged_index)
        return;

    if (dragged_index < new_index)
        new_index--;

    auto entry = m_entries.take(dragged_index);
    m_entries.insert(new_index, move(entry));

    for (size_t i = 0; i < m_entries.size(); i++)
        Config::write_string(CONFIG_DOMAIN, CONFIG_GROUP_ENTRIES, sanitize_name(m_entries[i]->name()), entry_to_config_string(i, m_entries[i]));
}

}
