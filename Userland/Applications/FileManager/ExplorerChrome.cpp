/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Vista/Win7 Explorer chrome. Everything is painted explicitly so the global
// Win2000 widget theme never leaks into the shell chrome (toolbars, address
// bar, navigation pane, preview pane, status bar).

#include <LibDesktop/DesktopIcons.h>
#include "ExplorerChrome.h"
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/FileIconProvider.h>
#include <LibGUI/Label.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextElision.h>
#include <sys/stat.h>
#include <time.h>

namespace FileManager {

namespace Aero {

// The Vista chrome is *not* an app-private colour table: every value comes
// from the system theme via Gfx::ColorRole. Switching the theme (or turning
// it off, back to the classic Redmond 2000 look) automatically repaints the
// command bar, address bar, navigation pane and status bar. The hard-coded
// values are only a fallback for the brief window before GUI::Application
// exists. See VISTA_THEME.md.
static Gfx::Color role(Gfx::ColorRole color_role, unsigned fallback)
{
    if (auto* app = GUI::Application::the())
        return app->palette().color(color_role);
    return Gfx::Color::from_rgb(fallback);
}

Gfx::Color chrome_top() { return role(Gfx::ColorRole::Window, 0xf0f4fa).lightened(1.04f); }
Gfx::Color chrome_bottom() { return role(Gfx::ColorRole::Button, 0xe2ebf7); }
Gfx::Color chrome_border() { return role(Gfx::ColorRole::ThreedShadow1, 0xb6c6dc); }
Gfx::Color nav_top() { return role(Gfx::ColorRole::Gutter, 0xf3f7fc).lightened(1.02f); }
Gfx::Color nav_bottom() { return role(Gfx::ColorRole::Window, 0xe9f0fa); }
Gfx::Color nav_hover() { return role(Gfx::ColorRole::HoverHighlight, 0xdcebfc); }
Gfx::Color nav_selected() { return role(Gfx::ColorRole::MenuSelection, 0xc6dcf8); }
Gfx::Color text() { return role(Gfx::ColorRole::WindowText, 0x1f2a37); }
Gfx::Color text_dim() { return role(Gfx::ColorRole::DisabledTextFront, 0x607086); }
Gfx::Color glass() { return role(Gfx::ColorRole::HoverHighlight, 0xe6f0ff); }
Gfx::Color white() { return role(Gfx::ColorRole::Window, 0xf0f4fa); }
Gfx::Color base() { return role(Gfx::ColorRole::Base, 0xffffff); }

}

static RefPtr<Gfx::Bitmap const> chrome_icon(StringView name)
{
    // Static: the pixels are compiled into the binary, so chrome icons never
    // depend on /res being present or on the image-decoder service.
    return Desktop::icon(name);
}

// ---------------------------------------------------------------------------
// ExplorerChromeButton
// ---------------------------------------------------------------------------

void ExplorerChromeButton::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());

    Gfx::Color background = Aero::white();
    if (m_checked)
        background = Aero::nav_selected();
    else if (m_pressed)
        background = Gfx::Color::from_rgb(0xbdd6f5);
    else if (m_hovered)
        background = Aero::nav_hover();

    if (m_hovered || m_pressed || m_checked) {
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(), background, background.darkened(0.94f));
        painter.draw_rect(rect(), Aero::chrome_border());
    } else {
        painter.fill_rect(rect(), Aero::white());
    }

    int const icon_size = 16;
    int x = 6;
    if (m_icon) {
        int y = (height() - icon_size) / 2;
        painter.draw_scaled_bitmap({ x, y, icon_size, icon_size }, *m_icon, m_icon->rect());
        x += icon_size + 5;
    } else if (!m_compact) {
        x = 8;
    }

    if (m_dropdown)
        x = max(x, 6);

    int right_reserve = m_dropdown ? 14 : 6;
    auto text_rect = Gfx::IntRect { x, 0, max(0, width() - x - right_reserve), height() };
    if (!m_compact && !m_text.is_empty() && text_rect.width() > 4) {
        painter.draw_text(text_rect, m_text, font(), Gfx::TextAlignment::CenterLeft,
            Aero::text(), Gfx::TextElision::Right);
    }

    if (m_dropdown) {
        int triangle_x = width() - 10;
        int triangle_y = height() / 2 - 1;
        for (int row = 0; row < 3; ++row)
            painter.draw_line({ triangle_x - row, triangle_y + row }, { triangle_x + row, triangle_y + row }, Aero::text_dim());
    }
}

void ExplorerChromeButton::mousemove_event(GUI::MouseEvent& event)
{
    m_hovered = rect().contains(event.position());
    update();
    GUI::Widget::mousemove_event(event);
}

void ExplorerChromeButton::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() == GUI::MouseButton::Primary)
        m_pressed = true;
    update();
    GUI::Widget::mousedown_event(event);
}

void ExplorerChromeButton::mouseup_event(GUI::MouseEvent& event)
{
    bool was_pressed = m_pressed;
    m_pressed = false;
    update();
    if (was_pressed && rect().contains(event.position())) {
        if (m_dropdown && on_dropdown_click)
            on_dropdown_click(screen_relative_rect().location().translated(event.position()));
        else if (on_click)
            on_click();
    }
    GUI::Widget::mouseup_event(event);
}

void ExplorerChromeButton::leave_event(Core::Event& event)
{
    m_hovered = false;
    m_pressed = false;
    update();
    GUI::Widget::leave_event(event);
}

void ExplorerChromeSeparator::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), Aero::white());
    int x = width() / 2;
    painter.draw_line({ x, 4 }, { x, height() - 5 }, Aero::chrome_border());
}

// ---------------------------------------------------------------------------
// ExplorerCommandBar
// ---------------------------------------------------------------------------

ExplorerCommandBar::ExplorerCommandBar()
{
    set_fill_with_background_color(true);
    set_fixed_height(32);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 3, 4, 3, 4 }, 2);
}

ExplorerChromeButton& ExplorerCommandBar::add_command(ByteString text, RefPtr<Gfx::Bitmap const> icon, bool dropdown)
{
    int button_width = 12 + static_cast<int>(font().width(text))
        + (icon ? 21 : 0) + (dropdown ? 18 : 0);

    auto& button = add<ExplorerChromeButton>();
    button.set_fill_with_background_color(true);
    button.set_icon(move(icon));
    button.set_dropdown(dropdown);
    button.set_text(move(text));
    button.set_fixed_height(26);
    button.set_fixed_width(button_width);
    return button;
}

void ExplorerCommandBar::add_separator()
{
    auto& separator = add<ExplorerChromeSeparator>();
    separator.set_fill_with_background_color(true);
    separator.set_fixed_width(7);
    separator.set_fixed_height(22);
}

void ExplorerCommandBar::add_stretch()
{
    auto& spacer = add<GUI::Widget>();
    spacer.set_fill_with_background_color(false);
    spacer.set_preferred_width(GUI::SpecialDimension::Grow);
}

void ExplorerCommandBar::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(), Aero::chrome_top(), Aero::chrome_bottom());
    painter.draw_line({ 0, height() - 1 }, { width() - 1, height() - 1 }, Aero::chrome_border());
    GUI::Widget::paint_event(event);
}

// ---------------------------------------------------------------------------
// ExplorerAddressFrame / ExplorerAddressBar
// ---------------------------------------------------------------------------

ExplorerAddressFrame::ExplorerAddressFrame()
{
    set_fill_with_background_color(true);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 2, 3, 2, 3 }, 0);
    m_host = add<GUI::Widget>();
    m_host->set_fill_with_background_color(false);
    m_host->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, 0);
}

void ExplorerAddressFrame::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), Aero::base());
    painter.draw_rect(rect().shrunken(0, 0), Aero::chrome_border());
    GUI::Widget::paint_event(event);
}

ExplorerAddressBar::ExplorerAddressBar()
{
    set_fill_with_background_color(true);
    set_fixed_height(34);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 3, 6, 3, 6 }, 2);

    auto make_nav_button = [this](StringView icon_name) -> ExplorerChromeButton& {
        auto& button = add<ExplorerChromeButton>();
        button.set_fill_with_background_color(true);
        button.set_compact(true);
        button.set_icon(chrome_icon(icon_name));
        button.set_fixed_width(28);
        button.set_fixed_height(26);
        return button;
    };

    m_back = make_nav_button("go-back"sv);
    m_forward = make_nav_button("go-forward"sv);
    m_up = make_nav_button("go-up"sv);
    m_refresh = make_nav_button("reload"sv);

    m_address_frame = add<ExplorerAddressFrame>();
    m_address_frame->set_preferred_width(GUI::SpecialDimension::Grow);
    m_address_frame->set_fixed_height(26);

    m_search_box = add<GUI::TextBox>();
    m_search_box->set_fixed_width(180);
    m_search_box->set_fixed_height(24);
    m_search_box->set_placeholder("搜索"sv);
}

GUI::Widget& ExplorerAddressBar::breadcrumb_host()
{
    return m_address_frame->host();
}

void ExplorerAddressBar::set_navigation_state(bool can_go_back, bool can_go_forward)
{
    m_back->set_enabled(can_go_back);
    m_forward->set_enabled(can_go_forward);
    update();
}

void ExplorerAddressBar::set_search_placeholder(StringView folder_name)
{
    auto label = folder_name.is_empty()
        ? ByteString { "搜索"sv }
        : ByteString::formatted("搜索 {}", folder_name);
    m_search_box->set_placeholder(label);
}

void ExplorerAddressBar::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(), Aero::chrome_top(), Aero::chrome_bottom());
    painter.draw_line({ 0, height() - 1 }, { width() - 1, height() - 1 }, Aero::chrome_border());
    GUI::Widget::paint_event(event);
}

// ---------------------------------------------------------------------------
// ExplorerNavigationPane
// ---------------------------------------------------------------------------

namespace {
constexpr int nav_group_header_height = 22;
constexpr int nav_item_height = 22;
constexpr int nav_indent = 14;
}

size_t ExplorerNavigationPane::add_group(ByteString title)
{
    Group group;
    group.title = move(title);
    m_groups.append(move(group));
    return m_groups.size() - 1;
}

void ExplorerNavigationPane::add_item(size_t group_index, ByteString title, ByteString path, RefPtr<Gfx::Bitmap const> icon)
{
    if (group_index >= m_groups.size())
        return;
    m_groups[group_index].items.append({ move(title), move(path), move(icon), {} });
    relayout();
    update();
}

void ExplorerNavigationPane::clear()
{
    m_groups.clear();
    m_hovered = {};
    m_pressed = {};
    relayout();
    update();
}

void ExplorerNavigationPane::set_current_path(ByteString path)
{
    if (m_current_path == path)
        return;
    m_current_path = move(path);
    update();
}

int ExplorerNavigationPane::content_height() const
{
    int height = 4;
    for (auto const& group : m_groups) {
        height += nav_group_header_height;
        if (!group.collapsed)
            height += static_cast<int>(group.items.size()) * nav_item_height;
    }
    return height + 4;
}

void ExplorerNavigationPane::relayout()
{
    int y = 4;
    for (auto& group : m_groups) {
        group.header_rect = { 0, y, width(), nav_group_header_height };
        y += nav_group_header_height;
        for (auto& item : group.items) {
            item.rect = { 0, y, width(), nav_item_height };
            y += nav_item_height;
        }
    }
}

ExplorerNavigationPane::Hit ExplorerNavigationPane::hit_test(Gfx::IntPoint point) const
{
    Hit hit;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        auto const& group = m_groups[g];
        if (group.header_rect.contains(point)) {
            hit.group = g;
            hit.group_header = true;
            return hit;
        }
        if (group.collapsed)
            continue;
        for (size_t i = 0; i < group.items.size(); ++i) {
            if (group.items[i].rect.contains(point)) {
                hit.group = g;
                hit.item = static_cast<ssize_t>(i);
                return hit;
            }
        }
    }
    return hit;
}

void ExplorerNavigationPane::paint_event(GUI::PaintEvent& event)
{
    relayout();
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(), Aero::nav_top(), Aero::nav_bottom());

    for (size_t g = 0; g < m_groups.size(); ++g) {
        auto const& group = m_groups[g];
        auto header = group.header_rect;
        painter.draw_text(header.translated(6, 0), group.title, font().bold_variant(),
            Gfx::TextAlignment::CenterLeft, Aero::text_dim());

        // Collapse triangle.
        int tx = width() - 12;
        int ty = header.center().y();
        if (group.collapsed) {
            for (int row = 0; row < 3; ++row)
                painter.draw_line({ tx - 2 + row, ty - 2 + row }, { tx - 2 + row, ty + 2 - row }, Aero::text_dim());
        } else {
            for (int row = 0; row < 3; ++row)
                painter.draw_line({ tx - 2, ty - 2 + row }, { tx + 2, ty - 2 + row }, Aero::text_dim());
        }

        if (group.collapsed)
            continue;

        for (size_t i = 0; i < group.items.size(); ++i) {
            auto const& item = group.items[i];
            bool selected = !m_current_path.is_empty() && item.path == m_current_path;
            bool hovered = m_hovered.group == g && m_hovered.item == static_cast<ssize_t>(i);
            if (selected || hovered) {
                auto fill = selected ? Aero::nav_selected() : Aero::nav_hover();
                painter.fill_rect(item.rect.shrunken(2, 0), fill);
                painter.draw_rect(item.rect.shrunken(2, 0), Aero::chrome_border());
            }

            int x = nav_indent;
            if (item.icon) {
                painter.draw_scaled_bitmap({ x, item.rect.y() + 3, 16, 16 }, *item.icon, item.icon->rect());
                x += 20;
            }
            auto text_rect = Gfx::IntRect { x, item.rect.y(), max(0, width() - x - 4), item.rect.height() };
            painter.draw_text(text_rect, item.title, font(), Gfx::TextAlignment::CenterLeft,
                Aero::text(), Gfx::TextElision::Right);
        }
    }

    painter.draw_line({ width() - 1, 0 }, { width() - 1, height() - 1 }, Aero::chrome_border());
}

void ExplorerNavigationPane::mousemove_event(GUI::MouseEvent& event)
{
    relayout();
    m_hovered = hit_test(event.position());
    update();
    GUI::Widget::mousemove_event(event);
}

void ExplorerNavigationPane::mousedown_event(GUI::MouseEvent& event)
{
    relayout();
    m_pressed = hit_test(event.position());
    update();
    GUI::Widget::mousedown_event(event);
}

void ExplorerNavigationPane::mouseup_event(GUI::MouseEvent& event)
{
    relayout();
    auto hit = hit_test(event.position());
    bool same = hit.group == m_pressed.group && hit.item == m_pressed.item && hit.group_header == m_pressed.group_header;
    m_pressed = {};
    if (same && hit.group < m_groups.size()) {
        auto& group = m_groups[hit.group];
        if (hit.group_header) {
            group.collapsed = !group.collapsed;
            update();
        } else if (hit.item >= 0 && static_cast<size_t>(hit.item) < group.items.size()) {
            auto const& path = group.items[hit.item].path;
            set_current_path(path);
            if (on_navigate)
                on_navigate(path);
        }
    }
    update();
    GUI::Widget::mouseup_event(event);
}

void ExplorerNavigationPane::leave_event(Core::Event& event)
{
    m_hovered = {};
    m_pressed = {};
    update();
    GUI::Widget::leave_event(event);
}

// ---------------------------------------------------------------------------
// ExplorerPreviewPane
// ---------------------------------------------------------------------------

void ExplorerPreviewPane::clear()
{
    set_selection({}, {});
}

void ExplorerPreviewPane::set_selection(Vector<ByteString> const& paths, ByteString const& current_directory)
{
    m_thumbnail.clear();
    m_headline = {};
    m_kind = {};
    m_size = {};
    m_modified = {};
    m_has_selection = !paths.is_empty();

    if (!m_has_selection) {
        m_hint = "选择一个项目以查看预览"sv;
        update();
        return;
    }

    auto const& path = paths.first();
    m_headline = paths.size() > 1
        ? ByteString::formatted("已选择 {} 个项目", paths.size())
        : LexicalPath::basename(path);

    struct stat st {};
    if (stat(path.characters(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            m_kind = "文件夹"sv;
            m_icon = GUI::FileIconProvider::directory_icon();
        } else {
            m_icon = GUI::FileIconProvider::icon_for_path(path);
            auto lower = path.to_lowercase();
            if (Gfx::Bitmap::is_path_a_supported_image_format(path)) {
                m_kind = "图片"sv;
                auto bitmap = Gfx::Bitmap::load_from_file(path);
                if (!bitmap.is_error())
                    m_thumbnail = bitmap.release_value();
            } else if (lower.ends_with(".zip"sv) || lower.ends_with(".tar.gz"sv) || lower.ends_with(".7z"sv)) {
                m_kind = "压缩文件"sv;
            } else if (lower.ends_with(".exe"sv) || lower.ends_with(".msi"sv)) {
                m_kind = "Windows 应用程序"sv;
            } else {
                m_kind = "文件"sv;
            }
            m_size = human_readable_size(static_cast<u64>(st.st_size)).to_byte_string();
        }
        char buffer[64] {};
        if (auto* tm = localtime(&st.st_mtime); tm && strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", tm))
            m_modified = buffer;
    } else {
        m_kind = "文件"sv;
    }

    if (m_kind.is_empty())
        m_kind = "文件"sv;
    if (!m_headline.is_empty() && current_directory.is_empty())
        m_hint = {};
    update();
}

void ExplorerPreviewPane::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), Aero::base());
    painter.draw_line({ 0, 0 }, { 0, height() - 1 }, Aero::chrome_border());

    auto area = rect().shrunken(10, 8);
    if (area.width() < 40 || area.height() < 40)
        return;

    if (!m_has_selection) {
        painter.draw_text(area, m_hint, font(), Gfx::TextAlignment::Center, Aero::text_dim());
        return;
    }

    // Vista gives the thumbnail all the room above the details block. The old
    // hard cap of 96px made a photo unreadable in a 190px pane, and made
    // widening the pane (drag the splitter) pointless.
    int const details_height = 22 + 18 * 4 + 12;
    int const box_height = max(96, area.height() - details_height);
    Gfx::IntRect preview_box { area.x(), area.y(), area.width(), box_height };

    if (m_thumbnail) {
        painter.fill_rect(preview_box, Color::White);
        painter.draw_rect(preview_box, Aero::chrome_border());
        auto inner = preview_box.shrunken(3, 3);
        auto source = m_thumbnail->rect();
        float scale = min(static_cast<float>(inner.width()) / source.width(),
            static_cast<float>(inner.height()) / source.height());
        // Never upscale past 1:1: blowing a 64px icon up to 200px looks worse
        // than letting it sit small and centred.
        scale = min(scale, 1.0f);
        Gfx::IntRect fitted { 0, 0, max(1, static_cast<int>(source.width() * scale)),
            max(1, static_cast<int>(source.height() * scale)) };
        fitted.center_within(inner);
        painter.draw_scaled_bitmap(fitted, *m_thumbnail, source);
    } else if (auto const* bitmap = m_icon.bitmap_for_size(48)) {
        Gfx::IntRect icon_rect { 0, 0, 48, 48 };
        icon_rect.center_within(preview_box);
        painter.draw_scaled_bitmap(icon_rect, *bitmap, bitmap->rect());
    }

    int y = preview_box.bottom() + 12;
    auto draw_line = [&](StringView label, StringView value, bool bold = false) {
        if (y + 18 > area.bottom())
            return;
        painter.draw_text(Gfx::IntRect { area.x(), y, area.width(), 16 }, label, font(),
            Gfx::TextAlignment::CenterLeft, Aero::text_dim(), Gfx::TextElision::Right);
        painter.draw_text(Gfx::IntRect { area.x() + 58, y, max(0, area.width() - 58), 16 }, value,
            bold ? font().bold_variant() : font(), Gfx::TextAlignment::CenterLeft,
            Aero::text(), Gfx::TextElision::Right);
        y += 18;
    };

    painter.draw_text(Gfx::IntRect { area.x(), y, area.width(), 18 }, m_headline, font().bold_variant(),
        Gfx::TextAlignment::CenterLeft, Aero::text(), Gfx::TextElision::Right);
    y += 22;
    painter.draw_line({ area.x(), y - 4 }, { area.right(), y - 4 }, Aero::chrome_border());
    draw_line("名称："sv, m_headline);
    draw_line("类型："sv, m_kind);
    if (!m_size.is_empty())
        draw_line("大小："sv, m_size);
    if (!m_modified.is_empty())
        draw_line("修改时间："sv, m_modified);
}

// ---------------------------------------------------------------------------
// ExplorerSplitterHandle
// ---------------------------------------------------------------------------

void ExplorerSplitterHandle::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), (m_hovered || m_dragging) ? Aero::nav_hover() : Aero::base());
    painter.draw_line({ 0, 0 }, { 0, height() - 1 }, Aero::chrome_border());
}

void ExplorerSplitterHandle::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::MouseButton::Primary)
        return;
    m_dragging = true;
    m_origin_x = event.x();
    if (on_drag_start)
        on_drag_start();
    update();
}

void ExplorerSplitterHandle::mousemove_event(GUI::MouseEvent& event)
{
    // LibGUI keeps delivering moves to the pressed widget, so the pointer may be
    // well outside this 5px strip while dragging; compute from the origin.
    if (!m_dragging)
        return;
    if (on_drag)
        on_drag(event.x() - m_origin_x);
}

void ExplorerSplitterHandle::mouseup_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::MouseButton::Primary || !m_dragging)
        return;
    m_dragging = false;
    update();
    if (on_drag_finished)
        on_drag_finished();
}

void ExplorerSplitterHandle::enter_event(Core::Event& event)
{
    m_hovered = true;
    set_override_cursor(Gfx::StandardCursor::ResizeColumn);
    update();
    GUI::Widget::enter_event(event);
}

void ExplorerSplitterHandle::leave_event(Core::Event& event)
{
    m_hovered = false;
    if (!m_dragging)
        set_override_cursor(Gfx::StandardCursor::None);
    update();
    GUI::Widget::leave_event(event);
}

// ---------------------------------------------------------------------------
// ExplorerViewSwitcher
// ---------------------------------------------------------------------------

Gfx::IntRect ExplorerViewSwitcher::button_rect(int index) const
{
    return { index * button_width, 0, button_width, height() };
}

namespace {

void draw_view_glyph(GUI::Painter& painter, int index, Gfx::IntRect rect, Gfx::Color color)
{
    auto box = rect.shrunken(6, 5);
    switch (index) {
    case 0: { // details: three rows of lines
        for (int row = 0; row < 3; ++row) {
            int y = box.y() + 2 + row * 4;
            painter.draw_line({ box.x(), y }, { box.right(), y }, color);
        }
        break;
    }
    case 1: { // large icons: four squares
        for (int row = 0; row < 2; ++row)
            for (int column = 0; column < 2; ++column)
                painter.fill_rect({ box.x() + column * 8, box.y() + row * 7, 6, 5 }, color);
        break;
    }
    case 2: { // tiles: square + two lines, twice
        for (int row = 0; row < 2; ++row) {
            int y = box.y() + row * 7;
            painter.fill_rect({ box.x(), y, 6, 5 }, color);
            painter.draw_line({ box.x() + 8, y + 1 }, { box.right(), y + 1 }, color);
            painter.draw_line({ box.x() + 8, y + 4 }, { box.right(), y + 4 }, color);
        }
        break;
    }
    default: { // preview pane toggle: split rectangle
        painter.draw_rect(box, color);
        painter.draw_line({ box.x() + box.width() * 2 / 3, box.y() }, { box.x() + box.width() * 2 / 3, box.bottom() }, color);
        break;
    }
    }
}

}

void ExplorerViewSwitcher::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect(rect(), Aero::white());
    for (int i = 0; i < 4; ++i) {
        auto button = button_rect(i);
        bool active = (i < 3 && static_cast<int>(m_active_view) == i) || (i == 3 && m_preview_visible);
        if (active || m_hovered == i) {
            auto color = active ? Aero::nav_selected() : Aero::nav_hover();
            painter.fill_rect(button.shrunken(1, 2), color);
            painter.draw_rect(button.shrunken(1, 2), Aero::chrome_border());
        }
        draw_view_glyph(painter, i, button, Aero::text());
    }
}

void ExplorerViewSwitcher::mousemove_event(GUI::MouseEvent& event)
{
    m_hovered = -1;
    for (int i = 0; i < 4; ++i) {
        if (button_rect(i).contains(event.position()))
            m_hovered = i;
    }
    update();
    GUI::Widget::mousemove_event(event);
}

void ExplorerViewSwitcher::mousedown_event(GUI::MouseEvent& event)
{
    m_pressed = -1;
    for (int i = 0; i < 4; ++i) {
        if (button_rect(i).contains(event.position()))
            m_pressed = i;
    }
    update();
    GUI::Widget::mousedown_event(event);
}

void ExplorerViewSwitcher::mouseup_event(GUI::MouseEvent& event)
{
    int pressed = m_pressed;
    m_pressed = -1;
    if (pressed >= 0 && button_rect(pressed).contains(event.position())) {
        if (pressed < 3) {
            m_active_view = static_cast<Mode>(pressed);
            if (on_view_change)
                on_view_change(m_active_view);
        } else {
            m_preview_visible = !m_preview_visible;
            if (on_preview_toggle)
                on_preview_toggle(m_preview_visible);
        }
    }
    update();
    GUI::Widget::mouseup_event(event);
}

void ExplorerViewSwitcher::leave_event(Core::Event& event)
{
    m_hovered = -1;
    m_pressed = -1;
    update();
    GUI::Widget::leave_event(event);
}

// ---------------------------------------------------------------------------
// ExplorerStatusbar
// ---------------------------------------------------------------------------

ExplorerStatusbar::ExplorerStatusbar()
{
    set_fill_with_background_color(true);
    set_fixed_height(24);
    set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 2, 6, 2, 6 }, 6);

    m_label = add<GUI::Label>();
    m_label->set_fill_with_background_color(false);
    m_label->set_text_alignment(Gfx::TextAlignment::CenterLeft);
    m_label->set_preferred_width(GUI::SpecialDimension::Grow);

    m_progressbar = add<GUI::Progressbar>();
    m_progressbar->set_fixed_width(120);
    m_progressbar->set_visible(false);

    m_switcher = add<ExplorerViewSwitcher>();
    m_switcher->set_fill_with_background_color(true);
    m_switcher->set_fixed_width(4 * 24);
    m_switcher->set_fixed_height(20);
}

void ExplorerStatusbar::set_text(String text)
{
    m_text = move(text);
    apply_text();
}

void ExplorerStatusbar::set_override_text(Optional<String> text)
{
    m_override_text = move(text);
    apply_text();
}

void ExplorerStatusbar::apply_text()
{
    m_label->set_text(m_override_text.has_value() ? m_override_text.value() : m_text);
}

void ExplorerStatusbar::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    // Only paint the damaged rect: GUI::Painter clips to the whole widget,
    // and Widget::handle_paint_event only repaints children that intersect
    // the damage rect. Filling rect() here would wipe every sibling that
    // was not hit by the damage (buttons vanished on horizontal sweeps).
    painter.add_clip_rect(event.rect());
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, rect(), Aero::chrome_top(), Aero::chrome_bottom());
    painter.draw_line({ 0, 0 }, { width() - 1, 0 }, Aero::chrome_border());
    // Win7's status bar deliberately has no resize grip.
    GUI::Widget::paint_event(event);
}

}
