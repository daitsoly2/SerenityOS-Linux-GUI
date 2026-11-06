/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Vista / Windows 7 Explorer chrome, drawn on top of LibGUI without touching
// the global (Win2000) widget theme.
//
//   ExplorerCommandBar     "组织 / 共享 / 新建文件夹 / 预览窗格" command bar
//   ExplorerAddressBar     back / forward / up, breadcrumb box, search box
//   ExplorerNavigationPane favourites / libraries / computer tree
//   ExplorerPreviewPane    large icon (or image thumbnail) + file details
//   ExplorerViewSwitcher   the small view buttons at the right of the status bar
//   ExplorerStatusbar      "N 个项目" + thumbnail progress + view buttons
//
// The widgets are deliberately independent of FileSystemModel: they take paths
// and callbacks, so the desktop shell could reuse them later without linking
// the file manager's model.

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Label.h>
#include <LibGUI/Progressbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Rect.h>

namespace FileManager {

// ---------------------------------------------------------------------------
// Shared palette. Aero's light shell chrome: near-white top, cool blue bottom.
// ---------------------------------------------------------------------------
namespace Aero {
Gfx::Color chrome_top();
Gfx::Color chrome_bottom();
Gfx::Color chrome_border();
Gfx::Color nav_top();
Gfx::Color nav_bottom();
Gfx::Color nav_hover();
Gfx::Color nav_selected();
Gfx::Color text();
Gfx::Color text_dim();
Gfx::Color glass();
Gfx::Color white();
Gfx::Color base();
}

// A flat, borderless Vista/Win7 command button. Hover/pressed states are drawn
// as translucent glass rectangles instead of Win9x 3D bevels.
class ExplorerChromeButton final : public GUI::Widget {
    C_OBJECT(ExplorerChromeButton)
public:
    void set_text(ByteString text) { m_text = move(text); update(); }
    void set_icon(RefPtr<Gfx::Bitmap const> icon) { m_icon = move(icon); update(); }
    void set_dropdown(bool b) { m_dropdown = b; update(); }
    void set_checked(bool b) { m_checked = b; update(); }
    bool is_checked() const { return m_checked; }
    void set_compact(bool b) { m_compact = b; update(); }

    Function<void()> on_click;
    Function<void(Gfx::IntPoint screen_position)> on_dropdown_click;

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;

private:
    ExplorerChromeButton() = default;

    ByteString m_text;
    RefPtr<Gfx::Bitmap const> m_icon;
    bool m_dropdown { false };
    bool m_compact { false };
    bool m_checked { false };
    bool m_hovered { false };
    bool m_pressed { false };
};

class ExplorerChromeSeparator final : public GUI::Widget {
    C_OBJECT(ExplorerChromeSeparator)
protected:
    virtual void paint_event(GUI::PaintEvent&) override;
private:
    ExplorerChromeSeparator() = default;
};

// The command bar (Win7's "Organize / Share / Burn / New folder" strip).
class ExplorerCommandBar final : public GUI::Widget {
    C_OBJECT(ExplorerCommandBar)
public:
    ExplorerChromeButton& add_command(ByteString text, RefPtr<Gfx::Bitmap const> icon = nullptr, bool dropdown = false);
    void add_separator();
    void add_stretch();

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
private:
    ExplorerCommandBar();
};

// A white rounded address box that hosts PathBreadcrumbbar.
class ExplorerAddressFrame final : public GUI::Widget {
    C_OBJECT(ExplorerAddressFrame)
public:
    GUI::Widget& host() { return *m_host; }

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
private:
    ExplorerAddressFrame();

    RefPtr<GUI::Widget> m_host;
};

// Back / forward / up buttons + breadcrumb + search box.
class ExplorerAddressBar final : public GUI::Widget {
    C_OBJECT(ExplorerAddressBar)
public:
    ExplorerChromeButton& back_button() { return *m_back; }
    ExplorerChromeButton& forward_button() { return *m_forward; }
    ExplorerChromeButton& up_button() { return *m_up; }
    ExplorerChromeButton& refresh_button() { return *m_refresh; }
    GUI::Widget& breadcrumb_host();
    GUI::TextBox& search_box() { return *m_search_box; }
    void set_navigation_state(bool can_go_back, bool can_go_forward);
    void set_search_placeholder(StringView folder_name);

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
private:
    ExplorerAddressBar();

    RefPtr<ExplorerChromeButton> m_back;
    RefPtr<ExplorerChromeButton> m_forward;
    RefPtr<ExplorerChromeButton> m_up;
    RefPtr<ExplorerChromeButton> m_refresh;
    RefPtr<ExplorerAddressFrame> m_address_frame;
    RefPtr<GUI::TextBox> m_search_box;
};

// Left navigation pane: collapsible groups with icon + label rows.
class ExplorerNavigationPane final : public GUI::Widget {
    C_OBJECT(ExplorerNavigationPane)
public:
    struct Item {
        ByteString title;
        ByteString path;
        RefPtr<Gfx::Bitmap const> icon;
        Gfx::IntRect rect;
    };
    struct Group {
        ByteString title;
        bool collapsed { false };
        Gfx::IntRect header_rect;
        Vector<Item> items;
    };

    size_t add_group(ByteString title);
    void add_item(size_t group, ByteString title, ByteString path, RefPtr<Gfx::Bitmap const> icon);
    void clear(); // drop all groups/items, for rebuilding the tree in place
    void set_current_path(ByteString path);
    int content_height() const;

    Function<void(ByteString const& path)> on_navigate;

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;

private:
    ExplorerNavigationPane() = default;

    struct Hit {
        size_t group { 0 };
        ssize_t item { -1 };
        bool group_header { false };
    };
    Hit hit_test(Gfx::IntPoint) const;
    void relayout();

    Vector<Group> m_groups;
    ByteString m_current_path;
    Hit m_hovered;
    Hit m_pressed;
};

// Right preview pane.
class ExplorerPreviewPane final : public GUI::Widget {
    C_OBJECT(ExplorerPreviewPane)
public:
    void set_selection(Vector<ByteString> const& paths, ByteString const& current_directory);
    void clear();

protected:
    virtual void paint_event(GUI::PaintEvent&) override;

private:
    ExplorerPreviewPane() = default;

    GUI::Icon m_icon;
    RefPtr<Gfx::Bitmap const> m_thumbnail;
    ByteString m_headline;
    ByteString m_kind;
    ByteString m_size;
    ByteString m_modified;
    ByteString m_hint;
    bool m_has_selection { false };
};

// The little status-bar buttons: details / large icons / tiles / preview.
class ExplorerViewSwitcher final : public GUI::Widget {
    C_OBJECT(ExplorerViewSwitcher)
public:
    enum Mode { Details = 0, Icons = 1, Tiles = 2 };

    void set_active_view(Mode mode) { m_active_view = mode; update(); }
    Mode active_view() const { return m_active_view; }
    void set_preview_visible(bool b) { m_preview_visible = b; update(); }
    bool preview_visible() const { return m_preview_visible; }

    Function<void(Mode)> on_view_change;
    Function<void(bool)> on_preview_toggle;

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;

private:
    ExplorerViewSwitcher() = default;

    static constexpr int button_width = 24;
    Gfx::IntRect button_rect(int index) const;

    Mode m_active_view { Details };
    bool m_preview_visible { false };
    int m_hovered { -1 };
    int m_pressed { -1 };
};

class ExplorerStatusbar final : public GUI::Widget {
    C_OBJECT(ExplorerStatusbar)
public:
    void set_text(String);
    void set_override_text(Optional<String>);
    GUI::Progressbar& progressbar() { return *m_progressbar; }
    ExplorerViewSwitcher& view_switcher() { return *m_switcher; }

protected:
    virtual void paint_event(GUI::PaintEvent&) override;

private:
    ExplorerStatusbar();

    RefPtr<GUI::Label> m_label;
    RefPtr<GUI::Progressbar> m_progressbar;
    RefPtr<ExplorerViewSwitcher> m_switcher;
    String m_text;
    Optional<String> m_override_text;
    void apply_text();
};

}
