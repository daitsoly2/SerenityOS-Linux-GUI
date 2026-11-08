/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Timer.h>
#include <AK/Vector.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Widget.h>
#include <LibGfx/Bitmap.h>

// The wallpaper + icon layer of the shell. Lives in the same process as the
// taskbar (see Shell/main.cpp), so both halves see the same AppCatalog.
class DesktopIconsWidget final : public GUI::Widget {
    C_OBJECT(DesktopIconsWidget)

public:
    virtual ~DesktopIconsWidget() override = default;

    // Icon size is user configurable (right-click -> 图标大小) and persisted in
    // ~/.config/Desktop.ini [Settings] IconSize. The cell adds room for the
    // label underneath, so it scales with the icon.
    int icon_size() const;
    int cell_width() const { return m_icon_size + 52; }
    int cell_height() const { return m_icon_size + 44; }
    void set_icon_size(int);

private:
    void load_icon_size();

    int m_icon_size { 32 };
    bool m_icon_size_loaded { false };

    struct Entry {
        String label;
        ByteString exec;
        Vector<ByteString> arguments;
        ByteString icon_path;
        ByteString path;        // optional real file/folder target (for open/compress)
        ByteString source_file; // the .af/.desktop file this entry came from
        RefPtr<Gfx::Bitmap const> icon;
        Gfx::IntRect rect;
        // Plain files and folders that live in ~/Desktop are shown too, but
        // "Remove icon" must not delete them - only shortcut files are ours.
        bool is_shortcut { true };
        // Target program is gone or the shortcut file no longer parses.
        bool broken { false };
    };

    // Descriptor for a shortcut file on the desktop (either .af or .desktop).
    // Built by PortShortcuts, so .desktop entries come from the converted .af
    // cache after the first scan and are not re-converted every refresh.
    struct Shortcut {
        ByteString name;
        ByteString exec;
        Vector<ByteString> arguments;
        ByteString icon_path;
        ByteString path;        // Path= (a file/folder the shortcut opens)
        ByteString source_file; // full path to the shortcut file
        bool broken { false };
    };

    static void scan_shortcuts(ByteString const& directory, Vector<Shortcut>& out);

    void load_shortcuts_into(Vector<Shortcut> const&);
    void load_plain_files_into(ByteString const& directory);
    void load_entries();
    void layout_entries();
    void ensure_desktop_watcher();
    void ensure_wallpaper_watcher();
    void reload_wallpaper_if_needed();

    // Icon positions are user state: ~/.config/Desktop.ini [Positions]
    //   Calculator.desktop=120,340
    static ByteString desktop_config_path();
    void load_positions(HashMap<ByteString, Gfx::IntPoint>&);
    void save_position(ByteString const& key, Gfx::IntPoint);
    void clear_positions();

    static void write_shortcut_file(ByteString const& name, ByteString const& exec, ByteString const& icon_path);

    RefPtr<Gfx::Bitmap const> wallpaper();

    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void doubleclick_event(GUI::MouseEvent&) override;
    virtual void context_menu_event(GUI::ContextMenuEvent&) override;
    virtual void drag_enter_event(GUI::DragEvent&) override;
    virtual void drop_event(GUI::DropEvent&) override;

    int entry_at(Gfx::IntPoint) const;
    bool is_selected(int) const;
    void clear_selection();
    void select_only(int);
    void launch_entry(Entry const&, bool force_game = false);
    void remove_icon_at(int index);
    void delete_entry_at(int index);
    void delete_entries(Vector<int> const& indices);
    void rename_entry_at(int index);
    void add_icon_menu(GUI::Menu&);
    void add_new_menu(GUI::Menu&);

    // "New > Folder" / "New > Text Document" write into ~/Desktop.
    void create_folder_on_desktop();
    void create_text_file_on_desktop();
    Vector<ByteString> template_paths();
    void create_file_from_template(ByteString const& template_path, StringView display_name, StringView extension);
    void reveal_new_entry(ByteString const& name);

    // Desktop context menu: drop the kernel's page cache (memory only - files
    // under ~/.cache belong to the programs that wrote them).
    ByteString clear_memory_cache();

    void start_drag_for_entry(Entry const&);

    // Desktop-local clipboard for 复制/剪切/粘贴. The system-wide clipboard
    // needs the ClipboardServer, which this port does not run yet.
    void copy_entry_to_clipboard(int index, bool cut);
    void copy_entries_to_clipboard(Vector<int> const& indices, bool cut);
    void paste_into_desktop();

    ByteString unique_desktop_name(StringView desired);
    ByteString file_target(Entry const&) const;
    static bool is_archive(ByteString const& path);
    static void show_information(StringView);
    static void show_error(StringView);
    static void run_tool(ByteString const& tool, Vector<ByteString> const& arguments, ByteString const& workdir);
    void compress_target(ByteString const& source, bool use_zip);
    void extract_target(ByteString const& source);

    Vector<Entry> m_entries;
    RefPtr<Gfx::Bitmap const> m_wallpaper;
    RefPtr<GUI::Menu> m_menu;
    bool m_wallpaper_loaded { false };
    ByteString m_wallpaper_path;
    ByteString m_wallpaper_mode;
    ByteString m_background_color;
    RefPtr<Core::Timer> m_wallpaper_timer;
    RefPtr<Core::FileWatcher> m_desktop_watcher;
    bool m_loaded { false };

    // Selection model: m_selection is the set of selected icon indices
    // (rubber-band box select + Ctrl-click toggle); m_selected is the
    // drag anchor / primary (kept inside m_selection). -1 means none.
    Vector<int> m_selection;
    int m_selected { -1 };

    // Rubber-band (marquee) selection state.
    bool m_marquee_active { false };
    Gfx::IntPoint m_marquee_start;
    Gfx::IntRect m_marquee_rect;

    // Icon dragging: press on an icon, move it while the pointer stays on the
    // desktop; leave the desktop and it turns into a drag-and-drop with the
    // entry's shortcut file as text/uri-list (that is how the quick launch bar
    // receives icons).
    Gfx::IntPoint m_press_position;
    Gfx::IntPoint m_grab_offset;
    Gfx::IntRect m_press_rect;
    bool m_might_drag { false };
    bool m_moving_icon { false };
    bool m_is_dragging { false };
    Gfx::IntPoint m_last_menu_position;
    Vector<ByteString> m_clipboard_paths;
    bool m_clipboard_is_cut { false };
};
