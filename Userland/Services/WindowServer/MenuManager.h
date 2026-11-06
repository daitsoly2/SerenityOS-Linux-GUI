/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Menu.h"
#include "Menubar.h"
#include "Window.h"
#include <AK/HashMap.h>
#include <AK/StringBuilder.h>

namespace WindowServer {

class MenuManager final : public Core::EventReceiver {
    C_OBJECT(MenuManager);

public:
    static MenuManager& the();

    virtual ~MenuManager() override = default;

    bool is_open(Menu const&) const;
    bool has_open_menu() const { return !m_open_menu_stack.is_empty(); }

    Menu* current_menu() { return m_current_menu.ptr(); }
    Menu* closest_open_ancestor_of(Menu const&) const;
    void set_current_menu(Menu*);
    void clear_current_menu();
    void open_menu(Menu&, bool as_current_menu = true);

    void close_everyone();
    void close_everyone_not_in_lineage(Menu&);
    void close_menu_and_descendants(Menu&);

    void close_all_menus_from_client(Badge<ConnectionFromClient>, ConnectionFromClient&);

    int theme_index() const { return m_theme_index; }

    Menu* previous_menu(Menu* current);
    Menu* next_menu(Menu* current);

    void did_change_theme();

    void set_hovered_menu(Menu*);
    Menu* hovered_menu() { return m_hovered_menu; }

private:
    MenuManager();

    void close_menus(Vector<Menu&>&);

    virtual void event(Core::Event&) override;
    void handle_mouse_event(MouseEvent&);

    // Linux GUI port: drag/right-click support for menu items.
    struct MenuHit {
        Menu* menu { nullptr };
        int menu_id { -1 };
        int identifier { -1 };
    };
    Menu* topmost_menu_at(Gfx::IntPoint screen_position) const;
    MenuHit hit_test(Gfx::IntPoint screen_position, bool require_item = true) const;
    void reset_item_drag();
    bool handle_item_drag_or_context_menu(MouseEvent&);

    void refresh();

    WeakPtr<Menu> m_current_menu;
    Vector<WeakPtr<Menu>> m_open_menu_stack;

    int m_theme_index { 0 };

    WeakPtr<Menu> m_hovered_menu;

    // A primary-button press on an item arms a drag; it only becomes a real
    // drag once the pointer has travelled more than a few pixels.
    bool m_item_drag_candidate { false };
    bool m_item_dragging { false };
    // Tracked from MouseDown/MouseUp instead of reading MouseEvent::buttons():
    // injected (QMP/libinput) pointer streams deliver motion events without
    // button state, which used to clear the candidate on the first move.
    bool m_item_drag_button_down { false };
    WeakPtr<Menu> m_item_drag_source;
    unsigned m_item_drag_source_identifier { 0 };
    Gfx::IntPoint m_item_drag_start;
};

}
