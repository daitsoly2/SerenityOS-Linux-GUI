/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <WindowServer/ConnectionFromClient.h>
#include <WindowServer/MenuManager.h>
#include <WindowServer/Screen.h>
#include <WindowServer/WindowManager.h>

namespace WindowServer {

static MenuManager* s_the;

MenuManager& MenuManager::the()
{
    VERIFY(s_the);
    return *s_the;
}

MenuManager::MenuManager()
{
    s_the = this;
}

bool MenuManager::is_open(Menu const& menu) const
{
    for (size_t i = 0; i < m_open_menu_stack.size(); ++i) {
        if (&menu == m_open_menu_stack[i].ptr())
            return true;
    }
    return false;
}

void MenuManager::refresh()
{
    ConnectionFromClient::for_each_client([&](ConnectionFromClient& client) {
        client.for_each_menu([&](Menu& menu) {
            menu.redraw();
            return IterationDecision::Continue;
        });
    });
}

void MenuManager::event(Core::Event& event)
{
    auto& wm = WindowManager::the();

    if (static_cast<Event&>(event).is_mouse_event()) {
        handle_mouse_event(static_cast<MouseEvent&>(event));
        return;
    }

    if (static_cast<Event&>(event).is_key_event()) {
        auto& key_event = static_cast<KeyEvent const&>(event);

        if (key_event.type() == Event::KeyUp && key_event.key() == Key_Escape) {
            close_everyone();
            return;
        }

        if (m_current_menu && event.type() == Event::KeyDown
            && ((key_event.key() >= Key_A && key_event.key() <= Key_Z)
                || (key_event.key() >= Key_0 && key_event.key() <= Key_9))) {

            if (auto* shortcut_item_indices = m_current_menu->items_with_alt_shortcut(key_event.code_point())) {
                VERIFY(!shortcut_item_indices->is_empty());
                auto it = shortcut_item_indices->find_if([&](int const& i) { return i > m_current_menu->hovered_item_index(); });
                auto index = shortcut_item_indices->at(it.is_end() ? 0 : it.index());
                auto& item = m_current_menu->item(index);
                m_current_menu->set_hovered_index(index);
                if (shortcut_item_indices->size() > 1)
                    return;
                if (item.is_submenu())
                    m_current_menu->descend_into_submenu_at_hovered_item();
                else
                    m_current_menu->open_hovered_item(false);
            }

            return;
        }

        if (event.type() == Event::KeyDown) {

            if (key_event.key() == Key_Left) {
                auto it = m_open_menu_stack.find_if([&](auto const& other) { return m_current_menu == other.ptr(); });
                VERIFY(!it.is_end());

                // Going "back" a menu should be the previous menu in the stack
                if (it.index() > 0)
                    set_current_menu(m_open_menu_stack.at(it.index() - 1));
                else {
                    if (m_current_menu->hovered_item())
                        m_current_menu->set_hovered_index(-1);
                    else {
                        auto* target_menu = previous_menu(m_current_menu);
                        if (target_menu) {
                            target_menu->ensure_menu_window(target_menu->rect_in_window_menubar().bottom_left().moved_up(1).translated(wm.window_with_active_menu()->frame().rect().location()).translated(wm.window_with_active_menu()->frame().menubar_rect().location()));
                            open_menu(*target_menu);
                            wm.window_with_active_menu()->invalidate_menubar();
                        }
                    }
                }
                close_everyone_not_in_lineage(*m_current_menu);
                return;
            }

            if (key_event.key() == Key_Right) {
                auto hovered_item = m_current_menu->hovered_item();
                if (hovered_item && hovered_item->is_submenu())
                    m_current_menu->descend_into_submenu_at_hovered_item();
                else if (m_open_menu_stack.size() <= 1 && wm.window_with_active_menu()) {
                    auto* target_menu = next_menu(m_current_menu);
                    if (target_menu) {
                        target_menu->ensure_menu_window(target_menu->rect_in_window_menubar().bottom_left().moved_up(1).translated(wm.window_with_active_menu()->frame().rect().location()).translated(wm.window_with_active_menu()->frame().menubar_rect().location()));
                        open_menu(*target_menu);
                        wm.window_with_active_menu()->invalidate_menubar();
                        close_everyone_not_in_lineage(*target_menu);
                    }
                }
                return;
            }

            if (key_event.key() == Key_Return) {
                auto hovered_item = m_current_menu->hovered_item();
                if (!hovered_item || !hovered_item->is_enabled())
                    return;
                if (hovered_item->is_submenu())
                    m_current_menu->descend_into_submenu_at_hovered_item();
                else
                    m_current_menu->open_hovered_item(key_event.modifiers() & KeyModifier::Mod_Ctrl);
                return;
            }

            if (key_event.key() == Key_Space) {
                auto* hovered_item = m_current_menu->hovered_item();
                if (!hovered_item || !hovered_item->is_enabled())
                    return;
                if (!hovered_item->is_checkable())
                    return;

                m_current_menu->open_hovered_item(true);
            }

            m_current_menu->dispatch_event(event);
        }
    }

    return Core::EventReceiver::event(event);
}

// --- Linux GUI port: start-menu item drag & drop / right-click ---------------
// WindowServer owns the menu windows, so the client cannot observe mouse events
// on menu items. These helpers translate the raw mouse stream into two IPC
// messages that the shell can act on (see WindowClient.ipc).

Menu* MenuManager::topmost_menu_at(Gfx::IntPoint screen_position) const
{
    for (int i = static_cast<int>(m_open_menu_stack.size()) - 1; i >= 0; --i) {
        auto* menu = m_open_menu_stack[i].ptr();
        if (!menu)
            continue;
        auto* window = menu->menu_window();
        if (window && window->is_visible() && window->rect().contains(screen_position))
            return menu;
    }
    return nullptr;
}

MenuManager::MenuHit MenuManager::hit_test(Gfx::IntPoint screen_position, bool require_item) const
{
    for (int i = static_cast<int>(m_open_menu_stack.size()) - 1; i >= 0; --i) {
        auto* menu = m_open_menu_stack[i].ptr();
        if (!menu)
            continue;
        auto* window = menu->menu_window();
        if (!window || !window->is_visible() || !window->rect().contains(screen_position))
            continue;
        MenuHit hit { menu, menu->menu_id(), -1 };
        int index = menu->item_index_at_screen_position(screen_position);
        if (index < 0)
            return require_item ? MenuHit {} : hit;
        auto& item = menu->item(static_cast<size_t>(index));
        if (item.type() == MenuItem::Separator || !item.is_enabled() || !item.is_visible())
            return require_item ? MenuHit {} : hit;
        hit.identifier = static_cast<int>(item.identifier());
        return hit;
    }
    return {};
}

void MenuManager::reset_item_drag()
{
    m_item_drag_candidate = false;
    m_item_dragging = false;
    m_item_drag_button_down = false;
    m_item_drag_source = nullptr;
    m_item_drag_source_identifier = 0;
    m_item_drag_start = {};
}

bool MenuManager::handle_item_drag_or_context_menu(MouseEvent& mouse_event)
{
    auto const position = mouse_event.position();

    if (mouse_event.type() == Event::MouseDown) {
        if (mouse_event.button() == MouseButton::Primary) {
            reset_item_drag();
            auto hit = hit_test(position);
            if (hit.menu) {
                m_item_drag_candidate = true;
                m_item_drag_button_down = true;
                m_item_drag_source = hit.menu;
                m_item_drag_source_identifier = static_cast<unsigned>(hit.identifier);
                m_item_drag_start = position;
            }
        } else if (mouse_event.button() == MouseButton::Secondary) {
            // Swallow the press inside a menu: without this the normal path
            // would close every menu on MouseDown, and the release would then
            // be delivered as a plain activation of the hovered item.
            if (topmost_menu_at(position))
                return true;
        }
        return false;
    }

    if (mouse_event.type() == Event::MouseMove) {
        if (!m_item_drag_candidate)
            return false;
        // Rely on the flag maintained by MouseDown/MouseUp rather than on the
        // button mask: injected pointer motion often carries no buttons at all.
        if (!m_item_drag_button_down) {
            reset_item_drag();
            return false;
        }
        if (!m_item_dragging && position.distance_from(m_item_drag_start) > 6.0f)
            m_item_dragging = true;
        // Deliberately fall through to the normal hover handling: hovering a
        // category item opens its submenu, which is the drop target we want.
        return false;
    }

    if (mouse_event.type() == Event::MouseUp) {
        // Secondary release => context menu, never activation.
        if (mouse_event.button() == MouseButton::Secondary) {
            auto hit = hit_test(position);
            reset_item_drag();
            if (hit.menu && hit.menu->client()) {
                hit.menu->client()->async_menu_item_context_menu(hit.menu_id,
                    static_cast<u32>(hit.identifier), position);
                // The client will show a confirmation dialog; menus must not
                // stay on top of it or swallow its input.
                close_everyone();
            }
            return true;
        }

        if (m_item_dragging) {
            auto* source = m_item_drag_source.ptr();
            auto source_identifier = m_item_drag_source_identifier;
            int target_menu_id = -1;
            int target_identifier = -1;
            if (auto* target = topmost_menu_at(position)) {
                target_menu_id = target->menu_id();
                if (target->menu_window()) {
                    int index = target->item_index_at_screen_position(position);
                    if (index >= 0)
                        target_identifier = static_cast<int>(target->item(static_cast<size_t>(index)).identifier());
                }
            }
            reset_item_drag();
            if (source && source->client()) {
                source->client()->async_menu_item_drag_dropped(source->menu_id(),
                    source_identifier, target_menu_id, target_identifier, position);
                close_everyone();
            }
            return true;
        }
        reset_item_drag();
    }
    return false;
}

void MenuManager::handle_mouse_event(MouseEvent& mouse_event)
{
    if (!has_open_menu())
        return;
    if (handle_item_drag_or_context_menu(mouse_event))
        return;
    auto* topmost_menu = m_open_menu_stack.last().ptr();
    VERIFY(topmost_menu);
    auto* window = topmost_menu->menu_window();
    if (!window) {
        dbgln("MenuManager::handle_mouse_event: No menu window");
        return;
    }
    VERIFY(window->is_visible());

    bool event_is_inside_current_menu = window->rect().contains(mouse_event.position());
    if (event_is_inside_current_menu) {
        WindowManager::the().set_hovered_window(window);
        WindowManager::the().deliver_mouse_event(*window, mouse_event);
        return;
    }

    if (topmost_menu->hovered_item())
        topmost_menu->clear_hovered_item();
    if (mouse_event.type() == Event::MouseDown || mouse_event.type() == Event::MouseUp) {
        auto* window_menu_of = topmost_menu->window_menu_of();
        if (window_menu_of) {
            bool event_is_inside_taskbar_button = window_menu_of->taskbar_rect().contains(mouse_event.position());
            if (event_is_inside_taskbar_button && !topmost_menu->is_window_menu_open()) {
                topmost_menu->set_window_menu_open(true);
                return;
            }
        }

        if (mouse_event.type() == Event::MouseDown) {
            for (auto& menu : m_open_menu_stack) {
                if (!menu)
                    continue;
                if (!menu->menu_window()->rect().contains(mouse_event.position()))
                    continue;
                return;
            }
            MenuManager::the().close_everyone();
            topmost_menu->set_window_menu_open(false);
        }
    }

    if (mouse_event.type() == Event::MouseMove) {
        for (auto& menu : m_open_menu_stack.in_reverse()) {
            if (!menu)
                continue;
            if (!menu->menu_window()->rect().contains(mouse_event.position()))
                continue;
            WindowManager::the().set_hovered_window(menu->menu_window());
            WindowManager::the().deliver_mouse_event(*menu->menu_window(), mouse_event);
            break;
        }
    }
}

void MenuManager::close_all_menus_from_client(Badge<ConnectionFromClient>, ConnectionFromClient& client)
{
    if (!has_open_menu())
        return;
    if (m_open_menu_stack.first()->client() != &client)
        return;
    close_everyone();
}

void MenuManager::close_everyone()
{
    for (auto& menu : m_open_menu_stack) {
        VERIFY(menu);
        menu->set_visible(false);
        menu->clear_hovered_item();
    }
    m_open_menu_stack.clear();
    clear_current_menu();
}

Menu* MenuManager::closest_open_ancestor_of(Menu const& other) const
{
    for (auto& menu : m_open_menu_stack.in_reverse())
        if (menu->is_menu_ancestor_of(other))
            return menu.ptr();
    return nullptr;
}

void MenuManager::close_everyone_not_in_lineage(Menu& menu)
{
    Vector<Menu&> menus_to_close;
    for (auto& open_menu : m_open_menu_stack) {
        if (!open_menu)
            continue;
        if (&menu == open_menu.ptr() || open_menu->is_menu_ancestor_of(menu))
            continue;
        menus_to_close.append(*open_menu);
    }
    close_menus(menus_to_close);
}

void MenuManager::close_menus(Vector<Menu&>& menus)
{
    for (auto& menu : menus) {
        if (&menu == m_current_menu)
            clear_current_menu();
        menu.set_visible(false);
        menu.clear_hovered_item();
        m_open_menu_stack.remove_first_matching([&](auto& entry) {
            return entry == &menu;
        });
    }
}

static void collect_menu_subtree(Menu& menu, Vector<Menu&>& menus)
{
    menus.append(menu);
    for (size_t i = 0; i < menu.item_count(); ++i) {
        auto& item = menu.item(i);
        if (!item.is_submenu())
            continue;
        collect_menu_subtree(*item.submenu(), menus);
    }
}

void MenuManager::close_menu_and_descendants(Menu& menu)
{
    Vector<Menu&> menus_to_close;
    collect_menu_subtree(menu, menus_to_close);
    close_menus(menus_to_close);
}

void MenuManager::set_hovered_menu(Menu* menu)
{
    if (m_hovered_menu == menu)
        return;
    if (menu) {
        m_hovered_menu = menu->make_weak_ptr<Menu>();
    } else {
        // FIXME: This is quite aggressive. If we knew which window the previously hovered menu was in,
        //        we could just invalidate that one instead of iterating all windows in the client.
        if (auto* client = m_hovered_menu->client()) {
            client->for_each_window([&](Window& window) {
                window.invalidate_menubar();
                return IterationDecision::Continue;
            });
        }
        m_hovered_menu = nullptr;
    }
}

void MenuManager::open_menu(Menu& menu, bool as_current_menu)
{
    if (menu.is_open()) {
        if (as_current_menu || current_menu() != &menu) {
            // This menu is already open. If requested, or if the current
            // window doesn't match this one, then set it to this
            set_current_menu(&menu);
        }
        return;
    }

    m_open_menu_stack.append(menu);

    menu.set_visible(true);

    if (!menu.is_empty()) {
        menu.redraw_if_theme_changed();
        auto* window = menu.menu_window();
        VERIFY(window);
        window->set_visible(true);
    }

    if (as_current_menu || !current_menu()) {
        // Only make this menu the current menu if requested, or if no
        // other menu is current
        set_current_menu(&menu);
    }
}

void MenuManager::clear_current_menu()
{
    if (m_current_menu) {
        auto& wm = WindowManager::the();
        if (auto* window = wm.window_with_active_menu()) {
            window->invalidate_menubar();
        }
        wm.set_window_with_active_menu(nullptr);
    }
    m_current_menu = nullptr;
}

void MenuManager::set_current_menu(Menu* menu)
{
    if (!menu) {
        clear_current_menu();
        return;
    }

    VERIFY(is_open(*menu));
    if (menu == m_current_menu) {
        return;
    }

    m_current_menu = menu;
}

Menu* MenuManager::previous_menu(Menu* current)
{
    auto& wm = WindowManager::the();
    if (!wm.window_with_active_menu())
        return nullptr;
    Menu* found = nullptr;
    Menu* previous = nullptr;
    wm.window_with_active_menu()->menubar().for_each_menu([&](Menu& menu) {
        if (current == &menu) {
            found = previous;
            return IterationDecision::Break;
        }
        previous = &menu;
        return IterationDecision::Continue;
    });
    return found;
}

Menu* MenuManager::next_menu(Menu* current)
{
    Menu* found = nullptr;
    bool is_next = false;
    auto& wm = WindowManager::the();
    if (!wm.window_with_active_menu())
        return nullptr;
    wm.window_with_active_menu()->menubar().for_each_menu([&](Menu& menu) {
        if (is_next) {
            found = &menu;
            return IterationDecision::Break;
        }
        if (current == &menu)
            is_next = true;
        return IterationDecision::Continue;
    });
    return found;
}

void MenuManager::did_change_theme()
{
    ++m_theme_index;
    refresh();
}

}
