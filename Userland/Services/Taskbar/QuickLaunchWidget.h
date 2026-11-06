/*
 * Copyright (c) 2021, Fabian Blatz <fabianblatz@gmail.com>
 * Copyright (c) 2023, David Ganz <david.g.ganz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibConfig/Listener.h>
#include <LibCore/FileWatcher.h>
#include <LibDesktop/AppFile.h>
#include <LibGUI/Button.h>
#include <LibGUI/Frame.h>
#include <LibGfx/Rect.h>

namespace Taskbar {

class QuickLaunchEntry {
public:
    static OwnPtr<QuickLaunchEntry> create_from_path(StringView path);

    virtual ~QuickLaunchEntry() = default;
    virtual ErrorOr<void> launch() const = 0;
    virtual GUI::Icon icon() const = 0;
    virtual ByteString name() const = 0;
    virtual ByteString file_name_to_watch() const = 0;

    virtual ByteString path() = 0;

    bool is_hovered() const { return m_hovered; }
    void set_hovered(bool hovered) { m_hovered = hovered; }

    void set_pressed(bool pressed) { m_pressed = pressed; }
    bool is_pressed() const { return m_pressed; }

private:
    bool m_hovered { false };
    bool m_pressed { false };
};

class QuickLaunchEntryAppFile : public QuickLaunchEntry {
public:
    explicit QuickLaunchEntryAppFile(NonnullRefPtr<Desktop::AppFile> file)
        : m_app_file(move(file))
    {
    }

    virtual ErrorOr<void> launch() const override;
    virtual GUI::Icon icon() const override;
    virtual ByteString name() const override { return m_app_file->name(); }
    virtual ByteString file_name_to_watch() const override { return {}; }

    virtual ByteString path() override { return m_app_file->filename(); }

private:
    NonnullRefPtr<Desktop::AppFile> m_app_file;
    // AppFile::icon() decodes the PNGs from disk every time it is called, and
    // the returned Icon owns the only reference to them. Caching keeps paint
    // cheap and, more importantly, keeps the bitmaps alive across the
    // bitmap_for_size() call in paint_event().
    mutable Optional<GUI::Icon> m_cached_icon;
};

class QuickLaunchEntryExecutable : public QuickLaunchEntry {
public:
    explicit QuickLaunchEntryExecutable(ByteString path)
        : m_path(move(path))
    {
    }

    virtual ErrorOr<void> launch() const override;
    virtual GUI::Icon icon() const override;
    virtual ByteString name() const override;
    virtual ByteString file_name_to_watch() const override { return m_path; }

    virtual ByteString path() override { return m_path; }

private:
    ByteString m_path;
};
class QuickLaunchEntryFile : public QuickLaunchEntry {
public:
    explicit QuickLaunchEntryFile(ByteString path)
        : m_path(move(path))
    {
    }

    virtual ErrorOr<void> launch() const override;
    virtual GUI::Icon icon() const override;
    virtual ByteString name() const override { return LexicalPath { m_path }.basename(); }
    virtual ByteString file_name_to_watch() const override { return m_path; }

    virtual ByteString path() override { return m_path; }

private:
    ByteString m_path;
};

// A freedesktop .desktop shortcut, which is what the desktop of this port puts
// on screen. Dragging one into the quick launch bar used to fall through to
// QuickLaunchEntryFile and produce an icon-less button named after the full
// path, so parse the Name/Exec/Icon keys instead.
class QuickLaunchEntryDesktopFile : public QuickLaunchEntry {
public:
    explicit QuickLaunchEntryDesktopFile(ByteString path);

    virtual ErrorOr<void> launch() const override;
    virtual GUI::Icon icon() const override;
    virtual ByteString name() const override { return m_name; }
    virtual ByteString file_name_to_watch() const override { return m_path; }

    virtual ByteString path() override { return m_path; }

private:
    ByteString m_path;
    ByteString m_name;
    ByteString m_icon_path;
    Vector<ByteString> m_arguments;
    mutable Optional<GUI::Icon> m_cached_icon;
};

class QuickLaunchWidget : public GUI::Frame
    , public Config::Listener {
    C_OBJECT(QuickLaunchWidget);

public:
    static ErrorOr<NonnullRefPtr<QuickLaunchWidget>> create();
    virtual ~QuickLaunchWidget() override = default;

    ErrorOr<bool> add_from_pid(pid_t pid);

protected:
    virtual void config_key_was_removed(StringView, StringView, StringView) override;
    virtual void config_string_did_change(StringView, StringView, StringView, StringView) override;

    virtual void drag_enter_event(GUI::DragEvent&) override;
    virtual void drop_event(GUI::DropEvent&) override;

    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void context_menu_event(GUI::ContextMenuEvent&) override;

    virtual void leave_event(Core::Event&) override;

    virtual void paint_event(GUI::PaintEvent&) override;
    // load_entries() runs before the taskbar lays us out, so the update() it
    // triggers lands on a 0-sized, not-yet-visible widget and is dropped. Paint
    // again once we actually have a size / become visible.
    virtual void resize_event(GUI::ResizeEvent&) override;
    virtual void show_event(GUI::ShowEvent&) override;

private:
    static constexpr StringView CONFIG_DOMAIN = "Taskbar"sv;
    static constexpr StringView CONFIG_GROUP_ENTRIES = "QuickLaunch_Entries"sv;
    static constexpr StringView OLD_CONFIG_GROUP_ENTRIES = "QuickLaunch"sv;
    static constexpr int MINIMUM_DRAG_DISTANCE = 6;
    static constexpr int BUTTON_SIZE = 24;

    explicit QuickLaunchWidget();

    ErrorOr<void> create_context_menu();

    void load_entries(bool save = true);
    ErrorOr<void> update_entry(ByteString const&, NonnullOwnPtr<QuickLaunchEntry>, bool save = true);
    void add_entries(Vector<NonnullOwnPtr<QuickLaunchEntry>> entries, bool save = true);

    template<typename Callback>
    void for_each_entry(Callback);

    void resize();

    void repaint();

    void set_or_insert_entry(NonnullOwnPtr<QuickLaunchEntry>, bool save = true);
    void remove_entry(ByteString const&, bool save = true);
    void recalculate_order();

    bool m_dragging { false };
    Gfx::IntPoint m_mouse_pos;
    int m_grab_offset { 0 };

    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Action> m_context_menu_default_action;
    ByteString m_context_menu_app_name;
    RefPtr<Core::FileWatcher> m_watcher;

    Vector<NonnullOwnPtr<QuickLaunchEntry>> m_entries;
};

}
