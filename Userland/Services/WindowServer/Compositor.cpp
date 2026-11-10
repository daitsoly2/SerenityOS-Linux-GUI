/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Animation.h"
#include "ConnectionFromClient.h"
#include "Event.h"
#include "EventLoop.h"
#include "MultiScaleBitmaps.h"
#include "Screen.h"
#include "Window.h"
#include "WindowManager.h"
#include "WindowSwitcher.h"
#include <AK/Debug.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <LibCore/Timer.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Painter.h>
#include <LibGfx/StylePainter.h>
#include <LibThreading/BackgroundAction.h>

namespace WindowServer {

Compositor& Compositor::the()
{
    static Compositor s_the;
    return s_the;
}

static WallpaperMode mode_to_enum(ByteString const& name)
{
    if (name == "Tile")
        return WallpaperMode::Tile;
    if (name == "Stretch")
        return WallpaperMode::Stretch;
    if (name == "Center")
        return WallpaperMode::Center;
    if (name == "Fill")
        return WallpaperMode::Fill;
    return WallpaperMode::Center;
}

Compositor::Compositor()
{
    m_display_link_notify_timer = add<Core::Timer>(
        1000 / 60, [this] {
            notify_display_links();
        });

    m_compose_timer = Core::Timer::create_single_shot(
        1000 / 60,
        [this] {
            compose();
        },
        this);
    m_compose_timer->start();

    m_immediate_compose_timer = Core::Timer::create_single_shot(
        0,
        [this] {
            compose();
        },
        this);
    m_compose_timer->start();

    init_bitmaps();
}

Gfx::Bitmap const* Compositor::cursor_bitmap_for_screenshot(Badge<ConnectionFromClient>, Screen& screen) const
{
    if (!m_current_cursor)
        return nullptr;
    return &m_current_cursor->bitmap(screen.scale_factor());
}

Gfx::Bitmap const& Compositor::front_bitmap_for_screenshot(Badge<ConnectionFromClient>, Screen& screen) const
{
    return *screen.compositor_screen_data().m_front_bitmap;
}

Gfx::Bitmap const& Compositor::front_bitmap_for_recorder(Badge<Recorder>, Screen& screen) const
{
    return *screen.compositor_screen_data().m_front_bitmap;
}

Gfx::Color Compositor::color_at_position(Badge<ConnectionFromClient>, Screen& screen, Gfx::IntPoint position) const
{
    return screen.compositor_screen_data().m_front_bitmap->get_pixel(position);
}

void CompositorScreenData::init_bitmaps(Compositor& compositor, Screen& screen)
{
    // Recreate the screen-number overlay as the Screen instances may have changed, or get rid of it if we no longer need it
    if (compositor.showing_screen_numbers()) {
        m_screen_number_overlay = compositor.create_overlay<ScreenNumberOverlay>(screen);
        m_screen_number_overlay->set_enabled(true);
    } else {
        m_screen_number_overlay = nullptr;
    }

    m_has_flipped = false;
    m_have_flush_rects = false;
    m_buffers_are_flipped = false;
    m_screen_can_set_buffer = screen.can_set_buffer();

    m_flush_rects.clear_with_capacity();
    m_flush_transparent_rects.clear_with_capacity();
    m_flush_special_rects.clear_with_capacity();

    auto size = screen.size();
    m_front_bitmap = nullptr;
    m_front_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRx8888, size, screen.scale_factor(), screen.pitch(), screen.scanline(0, 0)).release_value_but_fixme_should_propagate_errors();
    m_front_painter = make<Gfx::Painter>(*m_front_bitmap);
    m_front_painter->translate(-screen.rect().location());

    m_back_bitmap = nullptr;
    if (m_screen_can_set_buffer)
        m_back_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRx8888, size, screen.scale_factor(), screen.pitch(), screen.scanline(1, 0)).release_value_but_fixme_should_propagate_errors();
    else
        m_back_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, size, screen.scale_factor(), screen.pitch()).release_value_but_fixme_should_propagate_errors();
    m_back_painter = make<Gfx::Painter>(*m_back_bitmap);
    m_back_painter->translate(-screen.rect().location());

    m_temp_bitmap = nullptr;
    m_temp_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, size, screen.scale_factor(), screen.pitch()).release_value_but_fixme_should_propagate_errors();
    m_temp_painter = make<Gfx::Painter>(*m_temp_bitmap);
    m_temp_painter->translate(-screen.rect().location());

    clear_wallpaper_bitmap();
}

void Compositor::init_bitmaps()
{
    // The presenter decides this; recomputing it here also covers a presenter
    // switch (KMS -> wlroots) on a resolution change.
    if (m_use_window_layers)
        destroy_all_window_layers();
    m_use_window_layers = should_use_window_layers();

    Screen::for_each([&](auto& screen) {
        screen.compositor_screen_data().init_bitmaps(*this, screen);
        return IterationDecision::Continue;
    });

    invalidate_screen();
}

void Compositor::did_construct_window_manager(Badge<WindowManager>)
{
    auto& wm = WindowManager::the();

    m_current_window_stack = &wm.current_window_stack();

    m_wallpaper_mode = mode_to_enum(g_config->read_entry("Background", "Mode", "Center"));
    m_custom_background_color = Color::from_string(g_config->read_entry("Background", "Color", ""));

    invalidate_screen();
    invalidate_occlusions();
    compose();
}

Gfx::IntPoint Compositor::window_transition_offset(Window& window)
{
    if (WindowManager::is_stationary_window_type(window.type()))
        return {};

    if (window.is_moving_to_another_stack())
        return {};

    return window.window_stack().transition_offset();
}

void Compositor::compose()
{
    auto& wm = WindowManager::the();

    {
        auto& current_cursor = wm.active_cursor();
        if (m_current_cursor != &current_cursor) {
            change_cursor(&current_cursor);
            m_invalidated_cursor = m_invalidated_any = true;
        }
    }

    if (!m_invalidated_any) {
        // nothing dirtied since the last compose pass.
        return;
    }

    if (m_occlusions_dirty) {
        m_occlusions_dirty = false;
        recompute_occlusions();
    }

    // We should have recomputed occlusions if any overlay rects were changed
    VERIFY(!m_overlay_rects_changed);

    if (m_use_window_layers)
        sync_window_layers();

    auto dirty_screen_rects = move(m_dirty_screen_rects);

    bool window_stack_transition_in_progress = m_transitioning_to_window_stack != nullptr;

    // Mark window regions as dirty that need to be re-rendered
    wm.for_each_visible_window_from_back_to_front([&](Window& window) {
        auto transition_offset = window_transition_offset(window);
        auto frame_rect = window.frame().render_rect();
        auto frame_rect_on_screen = frame_rect.translated(transition_offset);
        for (auto& dirty_rect : dirty_screen_rects.rects()) {
            auto invalidate_rect = dirty_rect.intersected(frame_rect_on_screen);
            if (!invalidate_rect.is_empty()) {
                auto inner_rect_offset = window.rect().location() - frame_rect.location();
                invalidate_rect.translate_by(-(frame_rect.location() + inner_rect_offset + transition_offset));
                window.invalidate_no_notify(invalidate_rect);
                m_invalidated_window = true;
            }
        }
        window.prepare_dirty_rects();
        if (window_stack_transition_in_progress)
            window.dirty_rects().translate_by(transition_offset);
        return IterationDecision::Continue;
    });

    // Any dirty rects in transparency areas may require windows above or below
    // to also be marked dirty in these areas
    wm.for_each_visible_window_from_back_to_front([&](Window& window) {
        auto& dirty_rects = window.dirty_rects(); // dirty rects have already been adjusted for transition offset!
        if (dirty_rects.is_empty())
            return IterationDecision::Continue;
        auto& affected_transparency_rects = window.affected_transparency_rects();
        if (affected_transparency_rects.is_empty())
            return IterationDecision::Continue;
        // If we have transparency rects that affect others, we better have transparency rects ourselves...
        // ...but do not abort the whole compositor when that invariant slips:
        // a colour-keyed window whose content became fully transparent reports
        // no transparency rects of its own while the bookkeeping still lists
        // affected ones. Skipping the pass is harmless; crashing takes the
        // entire desktop down with it.
        auto& transparency_rects = window.transparency_rects();
        if (transparency_rects.is_empty())
            return IterationDecision::Continue;
        for (auto& it : affected_transparency_rects) {
            auto& affected_window_dirty_rects = it.key->dirty_rects();
            auto& affected_rects = it.value;
            affected_rects.for_each_intersected(dirty_rects, [&](auto& dirty_rect) {
                affected_window_dirty_rects.add(dirty_rect);
                return IterationDecision::Continue;
            });
        }
        return IterationDecision::Continue;
    });

    Color background_color = wm.palette().desktop_background();
    if (m_custom_background_color.has_value())
        background_color = m_custom_background_color.value();

    if constexpr (COMPOSE_DEBUG) {
        dbgln("COMPOSE: invalidated: window: {} cursor: {}, any: {}", m_invalidated_window, m_invalidated_cursor, m_invalidated_any);
        for (auto& r : dirty_screen_rects.rects())
            dbgln("dirty screen: {}", r);
    }

    auto& cursor_screen = ScreenInput::the().cursor_location_screen();

    Screen::for_each([&](auto& screen) {
        auto& screen_data = screen.compositor_screen_data();
        screen_data.m_have_flush_rects = false;
        screen_data.m_flush_rects.clear_with_capacity();
        screen_data.m_flush_transparent_rects.clear_with_capacity();
        screen_data.m_flush_special_rects.clear_with_capacity();
        // 诊断：把临时缓冲涂成洋红，任何「没画就回拷」的透明矩形都会露出来。
        if (getenv("SERENITY_TEMP_MAGENTA"))
            screen_data.m_temp_painter->fill_rect(screen.rect(), Gfx::Color::Magenta);
        return IterationDecision::Continue;
    });

    auto cursor_rect = current_cursor_rect();

    bool need_to_draw_cursor = false;
    Gfx::IntRect previous_cursor_rect;
    Screen* previous_cursor_screen = nullptr;
    auto check_restore_cursor_back = [&](Screen& screen, Gfx::IntRect const& rect) {
        if (&screen == &cursor_screen && !previous_cursor_screen && !need_to_draw_cursor && rect.intersects(cursor_rect)) {
            // Restore what's behind the cursor if anything touches the area of the cursor
            need_to_draw_cursor = true;
            if (cursor_screen.compositor_screen_data().restore_cursor_back(cursor_screen, previous_cursor_rect))
                previous_cursor_screen = &screen;
        }
    };

    if (&cursor_screen != m_current_cursor_screen) {
        // Cursor moved to another screen, restore on the cursor's background on the previous screen
        need_to_draw_cursor = true;
        if (m_current_cursor_screen) {
            if (m_current_cursor_screen->compositor_screen_data().restore_cursor_back(*m_current_cursor_screen, previous_cursor_rect))
                previous_cursor_screen = m_current_cursor_screen;
        }
        m_current_cursor_screen = &cursor_screen;
    }

    auto prepare_rect = [&](Screen& screen, Gfx::IntRect const& rect) {
        auto& screen_data = screen.compositor_screen_data();
        dbgln_if(COMPOSE_DEBUG, "    -> flush opaque: {}", rect);
        VERIFY(!screen_data.m_flush_rects.intersects(rect));
        VERIFY(!screen_data.m_flush_transparent_rects.intersects(rect));
        screen_data.m_have_flush_rects = true;
        screen_data.m_flush_rects.add(rect);
        check_restore_cursor_back(screen, rect);
    };

    auto prepare_transparency_rect = [&](Screen& screen, Gfx::IntRect const& rect) {
        auto& screen_data = screen.compositor_screen_data();
        dbgln_if(COMPOSE_DEBUG, "   -> flush transparent: {}", rect);
        VERIFY(!screen_data.m_flush_rects.intersects(rect));
        for (auto& r : screen_data.m_flush_transparent_rects.rects()) {
            if (r == rect)
                return;
        }

        screen_data.m_have_flush_rects = true;
        screen_data.m_flush_transparent_rects.add(rect);
        check_restore_cursor_back(screen, rect);
    };

    if (!cursor_screen.compositor_screen_data().m_cursor_back_bitmap || m_invalidated_cursor)
        check_restore_cursor_back(cursor_screen, cursor_rect);

    auto paint_wallpaper = [&](Screen& screen, Gfx::Painter& painter, Gfx::IntRect const& rect, Gfx::IntRect const& screen_rect) {
        // FIXME: If the wallpaper is opaque and covers the whole rect, no need to fill with color!
        painter.fill_rect(rect, background_color);
        if (m_wallpaper) {
            if (m_wallpaper_mode == WallpaperMode::Center) {
                Gfx::IntPoint offset { (screen.width() - m_wallpaper->width()) / 2, (screen.height() - m_wallpaper->height()) / 2 };
                painter.blit_offset(rect.location(), *m_wallpaper, rect.translated(-screen_rect.location()), offset);
            } else if (m_wallpaper_mode == WallpaperMode::Tile) {
                painter.draw_tiled_bitmap(rect, *m_wallpaper);
            } else if (m_wallpaper_mode == WallpaperMode::Stretch || m_wallpaper_mode == WallpaperMode::Fill) {
                // 壁纸位图可能因为分配失败而被清掉（见 update_wallpaper_bitmap）。
                // 此时背景色已经铺过，直接跳过图片即可，不能 VERIFY 掉整个服务器。
                if (auto const& wallpaper_bitmap = screen.compositor_screen_data().m_wallpaper_bitmap)
                    painter.blit(rect.location(), *wallpaper_bitmap, rect.translated(-screen.location()));
            } else {
                VERIFY_NOT_REACHED();
            }
        }
    };

    {
        // Paint any desktop wallpaper rects that are not somehow underneath any window transparency
        // rects and outside of any opaque window areas
        m_opaque_wallpaper_rects.for_each_intersected(dirty_screen_rects, [&](auto& render_rect) {
            Screen::for_each([&](auto& screen) {
                auto screen_rect = screen.rect();
                auto screen_render_rect = screen_rect.intersected(render_rect);
                if (!screen_render_rect.is_empty()) {
                    dbgln_if(COMPOSE_DEBUG, "  render wallpaper opaque: {} on screen #{}", screen_render_rect, screen.index());
                    prepare_rect(screen, render_rect);
                    auto& back_painter = *screen.compositor_screen_data().m_back_painter;
                    paint_wallpaper(screen, back_painter, render_rect, screen_rect);
                }
                return IterationDecision::Continue;
            });
            return IterationDecision::Continue;
        });
        m_transparent_wallpaper_rects.for_each_intersected(dirty_screen_rects, [&](auto& render_rect) {
            Screen::for_each([&](auto& screen) {
                auto screen_rect = screen.rect();
                auto screen_render_rect = screen_rect.intersected(render_rect);
                if (!screen_render_rect.is_empty()) {
                    dbgln_if(COMPOSE_DEBUG, "  render wallpaper transparent: {} on screen #{}", screen_render_rect, screen.index());
                    prepare_transparency_rect(screen, render_rect);
                    auto& temp_painter = *screen.compositor_screen_data().m_temp_painter;
                    paint_wallpaper(screen, temp_painter, render_rect, screen_rect);
                }
                return IterationDecision::Continue;
            });
            return IterationDecision::Continue;
        });
    }

    auto compose_window = [&](Window& window) -> IterationDecision {
        if (window.screens().is_empty()) {
            // This window doesn't intersect with any screens, so there's nothing to render
            return IterationDecision::Continue;
        }
        auto transition_offset = window_transition_offset(window);
        auto frame_rect = window.frame().render_rect().translated(transition_offset);
        auto window_rect = window.rect().translated(transition_offset);

        dbgln_if(COMPOSE_DEBUG, "  window {} frame rect: {}", window.title(), frame_rect);

        auto compose_window_rect = [&](Screen& screen, Gfx::Painter& painter, Gfx::IntRect const& rect) {
            render_window(painter, window, screen, rect, frame_rect, window_rect, transition_offset);
        };

        auto& dirty_rects = window.dirty_rects();

        if constexpr (COMPOSE_DEBUG) {
            for (auto& dirty_rect : dirty_rects.rects())
                dbgln("    dirty: {}", dirty_rect);
            for (auto& r : window.opaque_rects().rects())
                dbgln("    opaque: {}", r);
            for (auto& r : window.transparency_rects().rects())
                dbgln("    transparent: {}", r);
        }

        // Render opaque portions directly to the back buffer
        auto& opaque_rects = window.opaque_rects();
        if (!opaque_rects.is_empty()) {
            opaque_rects.for_each_intersected(dirty_rects, [&](Gfx::IntRect const& render_rect) {
                for (auto* screen : window.screens()) {
                    auto screen_render_rect = render_rect.intersected(screen->rect());
                    if (screen_render_rect.is_empty())
                        continue;
                    dbgln_if(COMPOSE_DEBUG, "    render opaque: {} on screen #{}", screen_render_rect, screen->index());

                    prepare_rect(*screen, screen_render_rect);
                    auto& back_painter = *screen->compositor_screen_data().m_back_painter;
                    Gfx::PainterStateSaver saver(back_painter);
                    back_painter.add_clip_rect(screen_render_rect);
                    compose_window_rect(*screen, back_painter, screen_render_rect);
                }
                return IterationDecision::Continue;
            });
        }

        // Render the wallpaper for any transparency directly covering
        // the wallpaper
        auto& transparency_wallpaper_rects = window.transparency_wallpaper_rects();
        if (!transparency_wallpaper_rects.is_empty()) {
            transparency_wallpaper_rects.for_each_intersected(dirty_rects, [&](Gfx::IntRect const& render_rect) {
                for (auto* screen : window.screens()) {
                    auto screen_rect = screen->rect();
                    auto screen_render_rect = render_rect.intersected(screen_rect);
                    if (screen_render_rect.is_empty())
                        continue;
                    dbgln_if(COMPOSE_DEBUG, "    render wallpaper: {} on screen #{}", screen_render_rect, screen->index());

                    auto& temp_painter = *screen->compositor_screen_data().m_temp_painter;
                    prepare_transparency_rect(*screen, screen_render_rect);
                    paint_wallpaper(*screen, temp_painter, screen_render_rect, screen_rect);
                }
                return IterationDecision::Continue;
            });
        }
        auto& transparency_rects = window.transparency_rects();
        if (!transparency_rects.is_empty()) {
            transparency_rects.for_each_intersected(dirty_rects, [&](Gfx::IntRect const& render_rect) {
                for (auto* screen : window.screens()) {
                    auto screen_rect = screen->rect();
                    auto screen_render_rect = render_rect.intersected(screen_rect);
                    if (screen_render_rect.is_empty())
                        continue;
                    dbgln_if(COMPOSE_DEBUG, "    render transparent: {} on screen #{}", screen_render_rect, screen->index());

                    prepare_transparency_rect(*screen, screen_render_rect);
                    auto& temp_painter = *screen->compositor_screen_data().m_temp_painter;
                    Gfx::PainterStateSaver saver(temp_painter);
                    temp_painter.add_clip_rect(screen_render_rect);
                    compose_window_rect(*screen, temp_painter, screen_render_rect);
                }
                return IterationDecision::Continue;
            });
        }
        return IterationDecision::Continue;
    };

    // Paint the window stack.
    if (m_invalidated_window) {
        if (m_use_window_layers) {
            // 拆层：桌面（壁纸/图标）照旧画进屏幕 framebuffer，作为最底下那一层；
            // 其余每个窗口各画进自己的 surface，由 wlroots 直接合成。窗口不再
            // 参与「整屏 framebuffer + 脏区」的契约，黑块就没有产生的土壤了。
            Vector<Window*, 32> layer_order;
            wm.for_each_visible_window_from_back_to_front([&](Window& window) {
                if (window.type() == WindowType::Desktop) {
                    compose_window(window);
                } else {
                    render_window_to_layer(window);
                    layer_order.append(&window);
                }
                window.clear_dirty_rects();
                return IterationDecision::Continue;
            });
            apply_window_layer_order(layer_order);
        } else {
            auto* fullscreen_window = wm.active_fullscreen_window();
            // FIXME: Remove the !WindowSwitcher::the().is_visible() check when WindowSwitcher is an overlay
            if (fullscreen_window && fullscreen_window->is_opaque() && !WindowSwitcher::the().is_visible()) {
                compose_window(*fullscreen_window);
                fullscreen_window->clear_dirty_rects();
            } else {
                wm.for_each_visible_window_from_back_to_front([&](Window& window) {
                    compose_window(window);
                    window.clear_dirty_rects();
                    return IterationDecision::Continue;
                });
            }
        }

        // Check that there are no overlapping transparent and opaque flush rectangles
        VERIFY(![&]() {
            bool is_overlapping = false;
            Screen::for_each([&](auto& screen) {
                auto& screen_data = screen.compositor_screen_data();
                auto& flush_transparent_rects = screen_data.m_flush_transparent_rects;
                auto& flush_rects = screen_data.m_flush_rects;
                for (auto& rect_transparent : flush_transparent_rects.rects()) {
                    for (auto& rect_opaque : flush_rects.rects()) {
                        if (rect_opaque.intersects(rect_transparent)) {
                            dbgln("Transparent rect {} overlaps opaque rect: {}: {}", rect_transparent, rect_opaque, rect_opaque.intersected(rect_transparent));
                            is_overlapping = true;
                            return IterationDecision::Break;
                        }
                    }
                }
                return IterationDecision::Continue;
            });
            return is_overlapping;
        }());

        if (!m_overlay_list.is_empty()) {
            // Render everything to the temporary buffer before we copy it back
            render_overlays();
        }

        // Copy anything rendered to the temporary buffer to the back buffer
        Screen::for_each([&](auto& screen) {
            auto screen_rect = screen.rect();
            auto& screen_data = screen.compositor_screen_data();
            if (getenv("SERENITY_TEMP_DEBUG") && !screen_data.m_flush_transparent_rects.is_empty()) {
                dbgln("COMPOSE: blit {} transparent rects", screen_data.m_flush_transparent_rects.rects().size());
                for (auto& rect : screen_data.m_flush_transparent_rects.rects())
                    dbgln("    transparent: {}", rect);
            }
            for (auto& rect : screen_data.m_flush_transparent_rects.rects())
                screen_data.m_back_painter->blit(rect.location(), *screen_data.m_temp_bitmap, rect.translated(-screen_rect.location()));
            return IterationDecision::Continue;
        });
    }

    m_invalidated_any = false;
    m_invalidated_window = false;
    m_invalidated_cursor = false;

    // A window layer whose buffers were all still being read by the renderer was
    // skipped this pass. Retry shortly instead of waiting for the next input event.
    if (m_window_layer_paint_pending) {
        m_window_layer_paint_pending = false;
        m_invalidated_window = true;
        m_invalidated_any = true;
        m_compose_timer->start();
    }

    if (!m_animations.is_empty()) {
        Screen::for_each([&](auto& screen) {
            auto& screen_data = screen.compositor_screen_data();
            update_animations(screen, screen_data.m_flush_special_rects);
            if (!screen_data.m_flush_special_rects.is_empty())
                screen_data.m_have_flush_rects = true;
            return IterationDecision::Continue;
        });
        // As long as animations are running make sure we keep rendering frames
        m_invalidated_any = true;
        start_compose_async_timer();
    }

    // SERENITY_NO_CURSOR: the embedded wlroots compositor draws the cursor itself
    // (and keeps it above every foreign window), so WindowServer must not paint a
    // second one into its own framebuffer - otherwise you see two cursors, and
    // the one baked into the desktop buffer can never sit above client windows.
    static bool const s_draw_cursor = getenv("SERENITY_NO_CURSOR") == nullptr;
    if (need_to_draw_cursor && s_draw_cursor) {
        auto& screen_data = cursor_screen.compositor_screen_data();
        screen_data.draw_cursor(cursor_screen, cursor_rect);
    }

    Screen::for_each([&](auto& screen) {
        flush(screen);
        return IterationDecision::Continue;
    });
}

// ── 拆层（window layers）────────────────────────────────────────────────────
// 只有真正的 compositor 后端（Linux 的 wlroots presenter）才提供窗口 surface；
// 其余后端一律走原来的「整屏 framebuffer」路径。
bool Compositor::should_use_window_layers() const
{
    if (getenv("SERENITY_NO_WINDOW_LAYERS"))
        return false;
    bool supported = false;
    Screen::for_each([&](auto& screen) {
        if (screen.supports_window_layers()) {
            supported = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return supported;
}

void Compositor::destroy_all_window_layers()
{
    for (auto& entry : m_window_layers) {
        if (entry.value.screen && entry.value.handle)
            entry.value.screen->destroy_window_layer(entry.value.handle);
    }
    m_window_layers.clear();
    m_window_layer_order.clear();
}

// 窗口销毁（或最小化）之后，它的 surface 必须跟着走：这里按「窗口栈里还
// 有没有它」来判断，避免在 Window 析构里挂钩子。
void Compositor::sync_window_layers()
{
    Vector<Window*, 64> live;
    for_each_rendering_window_stack([&](WindowStack& stack) {
        stack.for_each_window([&](Window& window) {
            live.append(&window);
            return IterationDecision::Continue;
        });
        stack.stationary_window_stack().for_each_window([&](Window& window) {
            live.append(&window);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });

    Vector<Window*, 8> dead;
    for (auto& entry : m_window_layers) {
        auto& state = entry.value;
        if (!live.contains_slow(entry.key)) {
            dead.append(entry.key);
            continue;
        }
        bool should_be_visible = entry.key->is_visible() && entry.key->type() != WindowType::Desktop;
        if (state.visible != should_be_visible) {
            if (state.screen && state.handle)
                state.screen->set_window_layer_enabled(state.handle, should_be_visible);
            state.visible = should_be_visible;
        }
        if (!should_be_visible)
            state.needs_paint = true;
    }

    for (auto* window : dead) {
        auto state = m_window_layers.take(window);
        if (state.has_value() && state->screen && state->handle)
            state->screen->destroy_window_layer(state->handle);
    }
}

void Compositor::apply_window_layer_order(Vector<Window*> const& order)
{
    bool changed = order.size() != m_window_layer_order.size();
    if (!changed) {
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] != m_window_layer_order[i]) {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
        return;

    // 从后往前 raise，最终顺序就等于窗口栈顺序（桌面层永远在最底下）。
    for (auto* window : order) {
        auto it = m_window_layers.find(window);
        if (it != m_window_layers.end() && it->value.screen && it->value.handle)
            it->value.screen->raise_window_layer(it->value.handle);
    }

    m_window_layer_order.clear_with_capacity();
    for (auto* window : order)
        m_window_layer_order.append(window);
}

void Compositor::render_window_to_layer(Window& window)
{
    if (window.screens().is_empty())
        return;
    auto* screen = window.screens().first();
    if (!screen || !screen->supports_window_layers())
        return;

    auto transition_offset = window_transition_offset(window);
    auto frame_rect = window.frame().render_rect().translated(transition_offset);
    if (frame_rect.is_empty())
        return;

    auto& state = m_window_layers.ensure(&window);
    if (state.handle && (state.screen != screen || state.frame_rect.size() != frame_rect.size())) {
        // 尺寸变了（或者换屏了）：旧的 surface 形状不对，重建。
        if (state.screen)
            state.screen->destroy_window_layer(state.handle);
        state.handle = nullptr;
    }

    if (!state.handle) {
        state.handle = screen->create_window_layer(frame_rect);
        state.screen = screen;
        state.frame_rect = frame_rect;
        state.needs_paint = true;
        state.visible = true;
        if (!state.handle)
            return;
        if (getenv("WSWLR_LAYER_DEBUG"))
            dbgln("ws-layer: create '{}' type={} rect={}", window.title(), (int)window.type(), frame_rect);
    }

    if (state.needs_paint || !window.dirty_rects().is_empty()) {
        auto scale = screen->scale_factor();
        auto physical_size = frame_rect.size() * scale;
        size_t stride = 0;
        auto* pixels = screen->begin_paint_window_layer(state.handle, physical_size, stride);
        if (!pixels) {
            // 这一层的两块缓冲都还在被渲染器读取：跳过这次，稍后重试。
            m_window_layer_paint_pending = true;
            return;
        }

        auto bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, frame_rect.size(), scale, stride, pixels)
                          .release_value_but_fixme_should_propagate_errors();
        {
            Gfx::Painter painter(*bitmap);
            // 层是带 alpha 的：先清成透明，窗口的圆角/阴影才能跟桌面混合。
            painter.clear_rect({ {}, frame_rect.size() }, Gfx::Color::Transparent);
            painter.translate(-frame_rect.location());
            render_window(painter, window, *screen, frame_rect, frame_rect,
                window.rect().translated(transition_offset), transition_offset);
        }
        screen->commit_window_layer(state.handle, frame_rect.location());
        state.frame_rect = frame_rect;
        state.needs_paint = false;
    } else if (state.frame_rect.location() != frame_rect.location()) {
        screen->move_window_layer(state.handle, frame_rect.location());
        state.frame_rect = frame_rect;
    }
}

void Compositor::render_window(Gfx::Painter& painter, Window& window, Screen& screen,
    Gfx::IntRect const& render_rect, Gfx::IntRect const& frame_rect,
    Gfx::IntRect const& window_rect, Gfx::IntPoint const& transition_offset)
{
    auto& wm = WindowManager::the();
    auto frame_rects = frame_rect.shatter(window_rect);

    if (!window.is_fullscreen()) {
        render_rect.for_each_intersected(frame_rects, [&](Gfx::IntRect const& intersected_rect) {
            Gfx::PainterStateSaver saver(painter);
            painter.add_clip_rect(intersected_rect);
            painter.translate(transition_offset);
            dbgln_if(COMPOSE_DEBUG, "    render frame: {}", intersected_rect);
            window.frame().paint(screen, painter, intersected_rect.translated(-transition_offset));
            return IterationDecision::Continue;
        });
    }

    auto update_window_rect = window_rect.intersected(render_rect);
    if (update_window_rect.is_empty())
        return;

    auto clear_window_rect = [&](Gfx::IntRect const& clear_rect) {
        // Transparent windows must not be padded with an opaque theme colour:
        // the area outside the backing store (resize gap, or no backing at all)
        // has to stay see-through so a shaped window keeps its silhouette.
        if (window.has_alpha_channel() || window.transparent_color().has_value())
            painter.clear_rect(clear_rect, Gfx::Color::Transparent);
        else
            painter.fill_rect(clear_rect, wm.palette().window());
    };

    RefPtr<Gfx::Bitmap> backing_store = window.backing_store();
    if (!backing_store) {
        clear_window_rect(update_window_rect);
        return;
    }

    // Decide where we would paint this window's backing store.
    // This is subtly different from widow.rect(), because window
    // size may be different from its backing store size. This
    // happens when the window has been resized and the client
    // has not yet attached a new backing store. In this case,
    // we want to try to blit the backing store at the same place
    // it was previously, and fill the rest of the window with its
    // background color.
    Gfx::IntRect backing_rect;
    backing_rect.set_size(window.backing_store_visible_size());
    switch (WindowManager::the().resize_direction_of_window(window)) {
    case ResizeDirection::None:
    case ResizeDirection::Right:
    case ResizeDirection::Down:
    case ResizeDirection::DownRight:
        backing_rect.set_location(window_rect.location());
        break;
    case ResizeDirection::Left:
    case ResizeDirection::Up:
    case ResizeDirection::UpLeft:
        backing_rect.set_right_without_resize(window_rect.right());
        backing_rect.set_bottom_without_resize(window_rect.bottom());
        break;
    case ResizeDirection::UpRight:
        backing_rect.set_left(window.rect().left());
        backing_rect.set_bottom_without_resize(window_rect.bottom());
        break;
    case ResizeDirection::DownLeft:
        backing_rect.set_right_without_resize(window_rect.right());
        backing_rect.set_top(window_rect.top());
        break;
    default:
        VERIFY_NOT_REACHED();
        break;
    }

    Gfx::IntRect dirty_rect_in_backing_coordinates = update_window_rect.intersected(backing_rect)
                                                         .translated(-backing_rect.location());

    if (!dirty_rect_in_backing_coordinates.is_empty()) {
        auto dst = backing_rect.location().translated(dirty_rect_in_backing_coordinates.location());

        if (window.client() && window.client()->is_unresponsive()) {
            painter.blit_filtered(dst, *backing_store, dirty_rect_in_backing_coordinates, [](Color src) {
                return src.to_grayscale().darkened(0.75f);
            });
        } else if (window.transparent_color().has_value()) {
            // Colour-key transparency: the skin bitmap marks holes with one
            // opaque RGB (M4Player's transparent_color). Filter those pixels
            // to fully transparent instead of blitting them.
            // NOTE: blit_filtered() leaves alpha=0 results untouched in the
            // destination, which is exactly what we want here - on the
            // framebuffer path the desktop is already painted underneath, so
            // skipped key pixels show the wallpaper. Do NOT clear_rect() the
            // dirty target first: the destination is opaque RGB and writing
            // zero there turns the holes black instead of see-through.
            painter.blit_filtered(dst, *backing_store, dirty_rect_in_backing_coordinates, [&window](Color src) {
                return window.matches_transparent_color(src) ? Gfx::Color::Transparent : src;
            });
        } else {
            painter.blit(dst, *backing_store, dirty_rect_in_backing_coordinates);
        }
    }

    for (auto background_rect : update_window_rect.shatter(backing_rect))
        clear_window_rect(background_rect);
}

void Compositor::flush(Screen& screen)
{
    auto& screen_data = screen.compositor_screen_data();

    bool device_can_flush_buffers = screen.can_device_flush_buffers();
    if (!screen_data.m_have_flush_rects && (!screen_data.m_screen_can_set_buffer || screen_data.m_has_flipped)) {
        dbgln_if(COMPOSE_DEBUG, "Nothing to flush on screen #{} {}", screen.index(), screen_data.m_have_flush_rects);
        return;
    }
    screen_data.m_have_flush_rects = false;

    auto screen_rect = screen.rect();
    if (m_flash_flush) {
        Gfx::IntRect bounding_flash;
        for (auto& rect : screen_data.m_flush_rects.rects()) {
            screen_data.m_front_painter->fill_rect(rect, Color::Yellow);
            bounding_flash = bounding_flash.united(rect);
        }
        for (auto& rect : screen_data.m_flush_transparent_rects.rects()) {
            screen_data.m_front_painter->fill_rect(rect, Color::Green);
            bounding_flash = bounding_flash.united(rect);
        }
        if (!bounding_flash.is_empty()) {
            if (screen.can_device_flush_entire_buffer()) {
                screen.flush_display_entire_framebuffer();
            } else if (device_can_flush_buffers) {
                // If the device needs a flush we need to let it know that we
                // modified the front buffer!
                bounding_flash.translate_by(-screen_rect.location());
                screen.flush_display_front_buffer((!screen_data.m_screen_can_set_buffer || !screen_data.m_buffers_are_flipped) ? 0 : 1, bounding_flash);
            }
            usleep(10000);
        }
    }

    if (device_can_flush_buffers && screen_data.m_screen_can_set_buffer) {
        if (!screen_data.m_has_flipped) {
            // If we have not flipped any buffers before, we should be flushing
            // the entire buffer to make sure that the device has all the bits we wrote
            screen_data.m_flush_rects = { screen.rect() };
        }

        // If we also support buffer flipping we need to make sure we transfer all
        // updated areas to the device before we flip. We already modified the framebuffer
        // memory, but the device needs to know what areas we actually did update.
        for (auto& rect : screen_data.m_flush_rects.rects())
            screen.queue_flush_display_rect(rect.translated(-screen_rect.location()));
        for (auto& rect : screen_data.m_flush_transparent_rects.rects())
            screen.queue_flush_display_rect(rect.translated(-screen_rect.location()));
        for (auto& rect : screen_data.m_flush_special_rects.rects())
            screen.queue_flush_display_rect(rect.translated(-screen_rect.location()));

        screen.flush_display((!screen_data.m_screen_can_set_buffer || screen_data.m_buffers_are_flipped) ? 0 : 1);
    }

    if (screen_data.m_screen_can_set_buffer) {
        screen_data.flip_buffers(screen);
        screen_data.m_has_flipped = true;
    }

    auto do_flush = [&](Gfx::IntRect rect) {
        // A window dragged partly off-screen (or resized past the edge) puts
        // dirty rects outside the screen. Aborting the compositor over that
        // is never the right answer - clip instead.
        if (!screen_rect.contains(rect)) {
            rect.intersect(screen_rect);
            if (rect.is_empty())
                return;
        }
        rect.translate_by(-screen_rect.location());

        // Almost everything in Compositor is in logical coordinates, with the painters having
        // a scale applied. But this routine accesses the backbuffer pixels directly, so it
        // must work in physical coordinates.
        auto scaled_rect = rect * screen.scale_factor();
        Gfx::ARGB32* front_ptr = screen_data.m_front_bitmap->scanline(scaled_rect.y()) + scaled_rect.x();
        Gfx::ARGB32* back_ptr = screen_data.m_back_bitmap->scanline(scaled_rect.y()) + scaled_rect.x();
        size_t pitch = screen_data.m_back_bitmap->pitch();

        // NOTE: The meaning of a flush depends on whether we can flip buffers or not.
        //
        //       If flipping is supported, flushing means that we've flipped, and now we
        //       copy the changed bits from the front buffer to the back buffer, to keep
        //       them in sync.
        //
        //       If flipping is not supported, flushing means that we copy the changed
        //       rects from the backing bitmap to the display framebuffer.

        Gfx::ARGB32* to_ptr;
        Gfx::ARGB32 const* from_ptr;

        if (screen_data.m_screen_can_set_buffer) {
            to_ptr = back_ptr;
            from_ptr = front_ptr;
        } else {
            to_ptr = front_ptr;
            from_ptr = back_ptr;
        }

        for (int y = 0; y < scaled_rect.height(); ++y) {
            fast_u32_copy(to_ptr, from_ptr, scaled_rect.width());
            from_ptr = (Gfx::ARGB32 const*)((u8 const*)from_ptr + pitch);
            to_ptr = (Gfx::ARGB32*)((u8*)to_ptr + pitch);
        }
        if (device_can_flush_buffers) {
            // Whether or not we need to flush buffers, we need to at least track what we modified
            // so that we can flush these areas next time before we flip buffers. Or, if we don't
            // support buffer flipping then we will flush them shortly.
            screen.queue_flush_display_rect(rect);
        }
    };
    for (auto& rect : screen_data.m_flush_rects.rects())
        do_flush(rect);
    for (auto& rect : screen_data.m_flush_transparent_rects.rects())
        do_flush(rect);
    for (auto& rect : screen_data.m_flush_special_rects.rects())
        do_flush(rect);
    if (device_can_flush_buffers && !screen_data.m_screen_can_set_buffer) {
        // If we also support flipping buffers we don't really need to flush these areas right now.
        // Instead, we skip this step and just keep track of them until shortly before the next flip.
        // If we however don't support flipping buffers then we need to flush the changed areas right
        // now so that they can be sent to the device.
        screen.flush_display(screen_data.m_buffers_are_flipped ? 1 : 0);
    }
}

void Compositor::invalidate_screen()
{
    invalidate_screen(Screen::bounding_rect());
}

void Compositor::invalidate_screen(Gfx::IntRect const& screen_rect)
{
    m_dirty_screen_rects.add(screen_rect.intersected(Screen::bounding_rect()));

    if (m_invalidated_any)
        return;

    m_invalidated_any = true;
    m_invalidated_window = true;
    start_compose_async_timer();
}

void Compositor::invalidate_screen(Gfx::DisjointIntRectSet const& rects)
{
    m_dirty_screen_rects.add(rects.intersected(Screen::bounding_rect()));

    if (m_invalidated_any)
        return;

    m_invalidated_any = true;
    m_invalidated_window = true;
    start_compose_async_timer();
}

void Compositor::invalidate_window()
{
    if (m_invalidated_window)
        return;
    m_invalidated_window = true;
    m_invalidated_any = true;

    start_compose_async_timer();
}

void Compositor::start_compose_async_timer()
{
    // We delay composition by a timer interval, but to not affect latency too
    // much, if a pending compose is not already scheduled, we also schedule an
    // immediate compose the next spin of the event loop.
    if (!m_compose_timer->is_active()) {
        m_compose_timer->start();
        m_immediate_compose_timer->start();
    }
}

bool Compositor::set_background_color(ByteString const& background_color)
{
    auto color = Color::from_string(background_color);
    if (!color.has_value())
        return false;

    m_custom_background_color = color;

    g_config->write_entry("Background", "Color", background_color);
    bool succeeded = !g_config->sync().is_error();

    if (succeeded) {
        update_wallpaper_bitmap();
        Compositor::invalidate_screen();
    }

    return succeeded;
}

bool Compositor::set_wallpaper_mode(ByteString const& mode)
{
    g_config->write_entry("Background", "Mode", mode);
    bool succeeded = !g_config->sync().is_error();

    if (succeeded) {
        m_wallpaper_mode = mode_to_enum(mode);
        update_wallpaper_bitmap();
        Compositor::invalidate_screen();
    }

    return succeeded;
}

bool Compositor::set_wallpaper(RefPtr<Gfx::Bitmap const> bitmap)
{
    if (!bitmap)
        m_wallpaper = nullptr;
    else
        m_wallpaper = bitmap;
    update_wallpaper_bitmap();
    invalidate_screen();

    return true;
}

void Compositor::update_wallpaper_bitmap()
{
    Screen::for_each([&](Screen& screen) {
        auto& screen_data = screen.compositor_screen_data();
        if ((m_wallpaper_mode != WallpaperMode::Stretch && m_wallpaper_mode != WallpaperMode::Fill) || !m_wallpaper) {
            screen_data.clear_wallpaper_bitmap();
            return IterationDecision::Continue;
        }

        // See if there is another screen with the same resolution and scale.
        // If so, we can use the same bitmap.
        bool share_bitmap_with_other_screen = false;
        Screen::for_each([&](Screen& screen2) {
            if (&screen == &screen2) {
                // Stop iterating here, we haven't updated wallpaper bitmaps for
                // this screen and the following screens.
                return IterationDecision::Break;
            }

            if (screen.size() == screen2.size() && screen.scale_factor() == screen2.scale_factor()) {
                auto& screen2_data = screen2.compositor_screen_data();

                // Use the same bitmap as the other screen
                screen_data.m_wallpaper_bitmap = screen2_data.m_wallpaper_bitmap;
                share_bitmap_with_other_screen = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });

        if (share_bitmap_with_other_screen)
            return IterationDecision::Continue;

        if (screen.size() == m_wallpaper->size() && screen.scale_factor() == m_wallpaper->scale()) {
            // If the screen size is equal to the wallpaper size, we don't actually need to scale it
            screen_data.m_wallpaper_bitmap = m_wallpaper;
        } else {
            // 这块位图的大小等于屏幕，分配失败说明内存已经非常紧张。此时保留
            // 纯背景色比 abort 整个 WindowServer 合理得多。
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, screen.size(), screen.scale_factor());
            if (bitmap_or_error.is_error()) {
                dbgln("Compositor: Could not allocate wallpaper bitmap for screen #{}: {}", screen.index(), bitmap_or_error.error());
                screen_data.clear_wallpaper_bitmap();
                return IterationDecision::Continue;
            }
            auto bitmap = bitmap_or_error.release_value();

            Gfx::Painter painter(*bitmap);

            if (m_wallpaper_mode == WallpaperMode::Stretch)
                painter.draw_scaled_bitmap(bitmap->rect(), *m_wallpaper, m_wallpaper->rect(), 1.f, Gfx::ScalingMode::BilinearBlend);

            if (m_wallpaper_mode == WallpaperMode::Fill) {
                auto wallpaper_rect = m_wallpaper->rect();
                auto stretched_rect = wallpaper_rect.scaled_to_cover(screen.rect());
                painter.draw_scaled_bitmap(stretched_rect, *m_wallpaper, wallpaper_rect, 1.0f, Gfx::ScalingMode::BilinearBlend);
            }

            screen_data.m_wallpaper_bitmap = move(bitmap);
        }
        return IterationDecision::Continue;
    });
}

void CompositorScreenData::clear_wallpaper_bitmap()
{
    m_wallpaper_bitmap = nullptr;
}

void CompositorScreenData::flip_buffers(Screen& screen)
{
    VERIFY(m_screen_can_set_buffer);
    swap(m_front_bitmap, m_back_bitmap);
    swap(m_front_painter, m_back_painter);
    screen.set_buffer(m_buffers_are_flipped ? 0 : 1);
    m_buffers_are_flipped = !m_buffers_are_flipped;
}

void Compositor::screen_resolution_changed()
{
    // Screens may be gone now, invalidate any references to them
    m_current_cursor_screen = nullptr;

    init_bitmaps();
    invalidate_occlusions();
    overlay_rects_changed();
    update_wallpaper_bitmap();
    compose();
}

Gfx::IntRect Compositor::current_cursor_rect() const
{
    auto& wm = WindowManager::the();
    auto& current_cursor = m_current_cursor ? *m_current_cursor : wm.active_cursor();
    Gfx::IntRect cursor_rect { ScreenInput::the().cursor_location().translated(-current_cursor.params().hotspot()), current_cursor.size() };
    if (wm.is_cursor_highlight_enabled()) {
        auto highlight_diameter = wm.cursor_highlight_radius() * 2;
        auto inflate_w = highlight_diameter - cursor_rect.width();
        auto inflate_h = highlight_diameter - cursor_rect.height();
        cursor_rect.inflate(inflate_w, inflate_h);
        // Ensures cursor stays in the same location when highlighting is enabled.
        cursor_rect.translate_by(-(inflate_w % 2), -(inflate_h % 2));
    }
    return cursor_rect;
}

void Compositor::invalidate_cursor(bool compose_immediately)
{
    if (m_invalidated_cursor && !compose_immediately)
        return;
    m_invalidated_cursor = true;
    m_invalidated_any = true;

    if (compose_immediately)
        compose();
    else
        start_compose_async_timer();
}

void Compositor::change_cursor(Cursor const* cursor)
{
    if (m_current_cursor == cursor)
        return;
    m_current_cursor = cursor;
    m_current_cursor_frame = 0;
    if (m_cursor_timer) {
        m_cursor_timer->stop();
        m_cursor_timer = nullptr;
    }
    if (cursor && cursor->params().frames() > 1 && cursor->params().frame_ms() != 0) {
        m_cursor_timer = add<Core::Timer>(
            cursor->params().frame_ms(), [this, cursor] {
                if (m_current_cursor != cursor)
                    return;
                auto frames = cursor->params().frames();
                if (++m_current_cursor_frame >= frames)
                    m_current_cursor_frame = 0;
                invalidate_cursor(true);
            });
        m_cursor_timer->start();
    }
}

void Compositor::render_overlays()
{
    // NOTE: overlays should always be rendered to the temporary buffer!
    for (auto& overlay : m_overlay_list) {
        for (auto* screen : overlay.m_screens) {
            auto& screen_data = screen->compositor_screen_data();
            auto& painter = screen_data.overlay_painter();

            auto render_overlay_rect = [&](auto& intersected_overlay_rect) {
                Gfx::PainterStateSaver saver(painter);
                painter.add_clip_rect(intersected_overlay_rect);
                painter.translate(overlay.m_current_rect.location());
                overlay.render(painter, *screen);
                return IterationDecision::Continue;
            };

            auto const& render_rect = overlay.current_render_rect();
            screen_data.for_each_intersected_flushing_rect(render_rect, render_overlay_rect);

            // Now render any areas that are not somehow underneath any window or transparency area
            m_transparent_wallpaper_rects.for_each_intersected(render_rect, render_overlay_rect);
        }
    }
}

void Compositor::add_overlay(Overlay& overlay)
{
    VERIFY(!overlay.m_list_node.is_in_list());
    auto zorder = overlay.zorder();
    bool did_insert = false;
    for (auto& other_overlay : m_overlay_list) {
        if (other_overlay.zorder() > zorder) {
            m_overlay_list.insert_before(other_overlay, overlay);
            did_insert = true;
            break;
        }
    }
    if (!did_insert)
        m_overlay_list.append(overlay);

    overlay.invalidate();
    overlay_rects_changed();
}

void Compositor::remove_overlay(Overlay& overlay)
{
    m_overlay_list.remove(overlay);

    auto last_rendered_rect = overlay.current_render_rect();
    if (!last_rendered_rect.is_empty()) {
        // We need to invalidate the entire area. While recomputing occlusions
        // will detect areas no longer occupied by overlays, if there are other
        // overlays intersecting with the overlay that was removed, then that
        // area would not get re-rendered.
        invalidate_screen(last_rendered_rect);
    }

    overlay_rects_changed();
}

void CompositorScreenData::draw_cursor(Screen& screen, Gfx::IntRect const& cursor_rect)
{
    auto& wm = WindowManager::the();

    if (!m_cursor_back_bitmap || m_cursor_back_bitmap->size() != cursor_rect.size() || m_cursor_back_bitmap->scale() != screen.scale_factor()) {
        m_cursor_back_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, cursor_rect.size(), screen.scale_factor()).release_value_but_fixme_should_propagate_errors();
        m_cursor_back_painter = make<Gfx::Painter>(*m_cursor_back_bitmap);
    }

    auto& compositor = Compositor::the();
    auto& current_cursor = compositor.m_current_cursor ? *compositor.m_current_cursor : wm.active_cursor();
    auto screen_rect = screen.rect();
    m_cursor_back_painter->blit({ 0, 0 }, *m_back_bitmap, cursor_rect.intersected(screen_rect).translated(-screen_rect.location()));
    auto cursor_src_rect = current_cursor.source_rect(compositor.m_current_cursor_frame);
    auto cursor_blit_pos = current_cursor.rect().centered_within(cursor_rect).location();

    if (wm.is_cursor_highlight_enabled()) {
        Gfx::AntiAliasingPainter aa_back_painter { *m_back_painter };
        aa_back_painter.fill_ellipse(cursor_rect, wm.cursor_highlight_color());
    }
    m_back_painter->blit(cursor_blit_pos, current_cursor.bitmap(screen.scale_factor()), cursor_src_rect);

    m_flush_special_rects.add(Gfx::IntRect(cursor_rect.location(), cursor_rect.size()).intersected(screen.rect()));
    m_have_flush_rects = true;
    m_last_cursor_rect = cursor_rect;
    VERIFY(compositor.m_current_cursor_screen == &screen);
    m_cursor_back_is_valid = true;
}

bool CompositorScreenData::restore_cursor_back(Screen& screen, Gfx::IntRect& last_cursor_rect)
{
    if (!m_cursor_back_is_valid || !m_cursor_back_bitmap || m_cursor_back_bitmap->scale() != m_back_bitmap->scale())
        return false;

    last_cursor_rect = m_last_cursor_rect.intersected(screen.rect());
    m_back_painter->blit(last_cursor_rect.location(), *m_cursor_back_bitmap, { { 0, 0 }, last_cursor_rect.size() });
    m_flush_special_rects.add(last_cursor_rect.intersected(screen.rect()));
    m_have_flush_rects = true;
    m_cursor_back_is_valid = false;
    return true;
}

void Compositor::update_fonts()
{
    ScreenNumberOverlay::pick_font();
}

void Compositor::notify_display_links()
{
    ConnectionFromClient::for_each_client([](auto& client) {
        client.notify_display_link({});
    });
}

void Compositor::increment_display_link_count(Badge<ConnectionFromClient>)
{
    ++m_display_link_count;
    if (m_display_link_count == 1)
        m_display_link_notify_timer->start();
}

void Compositor::decrement_display_link_count(Badge<ConnectionFromClient>)
{
    VERIFY(m_display_link_count);
    --m_display_link_count;
    if (!m_display_link_count)
        m_display_link_notify_timer->stop();
}

void Compositor::invalidate_current_screen_number_rects()
{
    Screen::for_each([&](auto& screen) {
        auto& screen_data = screen.compositor_screen_data();
        if (screen_data.m_screen_number_overlay)
            screen_data.m_screen_number_overlay->invalidate();
        return IterationDecision::Continue;
    });
}

void Compositor::increment_show_screen_number(Badge<ConnectionFromClient>)
{
    if (m_show_screen_number_count++ == 0) {
        Screen::for_each([&](auto& screen) {
            auto& screen_data = screen.compositor_screen_data();
            VERIFY(!screen_data.m_screen_number_overlay);
            screen_data.m_screen_number_overlay = create_overlay<ScreenNumberOverlay>(screen);
            screen_data.m_screen_number_overlay->set_enabled(true);
            return IterationDecision::Continue;
        });
    }
}
void Compositor::decrement_show_screen_number(Badge<ConnectionFromClient>)
{
    if (--m_show_screen_number_count == 0) {
        invalidate_current_screen_number_rects();
        Screen::for_each([&](auto& screen) {
            screen.compositor_screen_data().m_screen_number_overlay = nullptr;
            return IterationDecision::Continue;
        });
    }
}

void Compositor::overlays_theme_changed()
{
    for (auto& overlay : m_overlay_list)
        overlay.theme_changed();
    overlay_rects_changed();
}

void Compositor::overlay_rects_changed()
{
    if (m_overlay_rects_changed)
        return;

    m_overlay_rects_changed = true;
    m_invalidated_any = true;
    invalidate_occlusions();
    start_compose_async_timer();
}

void Compositor::recompute_overlay_rects()
{
    // The purpose of this is to gather all areas that we will render over
    // regular window contents. This effectively just forces those areas to
    // be rendered as transparency areas, which allows us to render these
    // flicker-free.
    swap(m_last_rendered_overlay_rects, m_overlay_rects);
    m_overlay_rects.clear_with_capacity();
    for (auto& overlay : m_overlay_list) {
        auto& render_rect = overlay.rect();
        m_overlay_rects.add(render_rect);

        // Invalidate areas that are no longer in the rendered area because the overlay was moved.
        auto previous_rects = overlay.current_render_rect().shatter(render_rect);
        for (auto& rect : previous_rects)
            invalidate_screen(rect);

        // Save the rectangle we are using for rendering from now on
        bool needs_invalidation = overlay.apply_render_rect();

        // Cache which screens this overlay are rendered on
        overlay.m_screens.clear_with_capacity();
        Screen::for_each([&](auto& screen) {
            if (render_rect.intersects(screen.rect()))
                overlay.m_screens.append(&screen);
            return IterationDecision::Continue;
        });

        if (needs_invalidation)
            invalidate_screen(render_rect);
    }

    // Invalidate rects that are not going to get rendered anymore, e.g.
    // because overlays were removed or rectangles were changed
    auto no_longer_rendered_rects = m_last_rendered_overlay_rects.shatter(m_overlay_rects);
    for (auto& rect : no_longer_rendered_rects.rects())
        invalidate_screen(rect);
}

void Compositor::recompute_occlusions()
{
    auto& wm = WindowManager::the();
    bool is_switcher_visible = wm.m_switcher->is_visible();
    auto never_occlude = [&](WindowStack& window_stack) {
        if (is_switcher_visible) {
            switch (wm.m_switcher->mode()) {
            case WindowSwitcher::Mode::ShowCurrentDesktop:
                // Any window on the currently rendered desktop should not be occluded, even if it's behind
                // another window entirely.
                return &window_stack == m_current_window_stack || &window_stack == m_transitioning_to_window_stack;
            case WindowSwitcher::Mode::ShowAllWindows:
                // The window switcher wants to know about all windows, even those on other desktops
                return true;
            }
        }
        return false;
    };

    wm.for_each_window_stack([&](WindowStack& window_stack) {
        if (&window_stack == m_current_window_stack || &window_stack == m_transitioning_to_window_stack) {
            // We'll calculate precise occlusions for these further down. Changing occlusions right now
            // may trigger an additional unnecessary notification
        } else {
            window_stack.set_all_occluded(!never_occlude(window_stack));
        }
        return IterationDecision::Continue;
    });

    if (m_overlay_rects_changed) {
        m_overlay_rects_changed = false;
        recompute_overlay_rects();
    }

    if constexpr (OCCLUSIONS_DEBUG) {
        dbgln("OCCLUSIONS:");
        for (auto& rect : m_overlay_rects.rects())
            dbgln("  overlay: {}", rect);
    }

    bool window_stack_transition_in_progress = m_transitioning_to_window_stack != nullptr;
    auto& main_screen = Screen::main();
    auto* fullscreen_window = wm.active_fullscreen_window();
    // FIXME: Remove the !WindowSwitcher::the().is_visible() check when WindowSwitcher is an overlay
    if (fullscreen_window && !WindowSwitcher::the().is_visible()) {
        // TODO: support fullscreen windows on all screens
        auto screen_rect = main_screen.rect();
        wm.for_each_visible_window_from_front_to_back([&](Window& w) {
            auto& visible_opaque = w.opaque_rects();
            auto& transparency_rects = w.transparency_rects();
            auto& transparency_wallpaper_rects = w.transparency_wallpaper_rects();
            w.affected_transparency_rects().clear();
            w.screens().clear_with_capacity();
            if (&w == fullscreen_window) {
                w.screens().append(&main_screen);
                if (w.is_opaque()) {
                    visible_opaque = screen_rect;
                    transparency_rects.clear();
                    transparency_wallpaper_rects.clear();
                } else {
                    visible_opaque.clear();
                    transparency_rects = screen_rect;
                    transparency_wallpaper_rects = screen_rect;
                }
            } else {
                visible_opaque.clear();
                transparency_rects.clear();
                transparency_wallpaper_rects.clear();
            }
            return IterationDecision::Continue;
        });

        m_opaque_wallpaper_rects.clear();
    }
    // FIXME: Remove the WindowSwitcher::the().is_visible() check when WindowSwitcher is an overlay
    if (!fullscreen_window || WindowSwitcher::the().is_visible() || (fullscreen_window && !fullscreen_window->is_opaque())) {
        Gfx::DisjointIntRectSet remaining_visible_screen_rects;
        remaining_visible_screen_rects.add_many(Screen::rects());
        bool have_transparent = false;
        wm.for_each_visible_window_from_front_to_back([&](Window& w) {
            VERIFY(!w.is_minimized());
            w.transparency_wallpaper_rects().clear();
            auto previous_visible_opaque = move(w.opaque_rects());
            auto previous_visible_transparency = move(w.transparency_rects());

            auto invalidate_previous_render_rects = [&](Gfx::IntRect const& new_render_rect) {
                if (!previous_visible_opaque.is_empty()) {
                    if (new_render_rect.is_empty())
                        invalidate_screen(previous_visible_opaque);
                    else
                        invalidate_screen(previous_visible_opaque.shatter(new_render_rect));
                }
                if (!previous_visible_transparency.is_empty()) {
                    if (new_render_rect.is_empty())
                        invalidate_screen(previous_visible_transparency);
                    else
                        invalidate_screen(previous_visible_transparency.shatter(new_render_rect));
                }
            };

            auto& visible_opaque = w.opaque_rects();
            auto& transparency_rects = w.transparency_rects();
            bool should_invalidate_old = w.should_invalidate_last_rendered_screen_rects();

            auto& affected_transparency_rects = w.affected_transparency_rects();
            affected_transparency_rects.clear();

            w.screens().clear_with_capacity();

            auto transition_offset = window_transition_offset(w);
            auto transparent_frame_render_rects = w.frame().transparent_render_rects();
            auto opaque_frame_render_rects = w.frame().opaque_render_rects();
            if (window_stack_transition_in_progress) {
                transparent_frame_render_rects.translate_by(transition_offset);
                opaque_frame_render_rects.translate_by(transition_offset);
            }
            if (should_invalidate_old) {
                for (auto& rect : opaque_frame_render_rects.rects())
                    invalidate_previous_render_rects(rect);
                for (auto& rect : transparent_frame_render_rects.rects())
                    invalidate_previous_render_rects(rect);
            }

            if (auto transparent_render_rects = transparent_frame_render_rects.intersected(remaining_visible_screen_rects); !transparent_render_rects.is_empty())
                transparency_rects = move(transparent_render_rects);
            if (auto opaque_render_rects = opaque_frame_render_rects.intersected(remaining_visible_screen_rects); !opaque_render_rects.is_empty())
                visible_opaque = move(opaque_render_rects);

            auto render_rect_on_screen = w.frame().render_rect().translated(transition_offset);
            auto visible_window_rects = remaining_visible_screen_rects.intersected(w.rect().translated(transition_offset));
            Gfx::DisjointIntRectSet opaque_covering;
            Gfx::DisjointIntRectSet transparent_covering;
            bool found_this_window = false;
            wm.for_each_visible_window_from_back_to_front([&](Window& w2) {
                if (!found_this_window) {
                    if (&w == &w2)
                        found_this_window = true;
                    return IterationDecision::Continue;
                }

                VERIFY(!w2.is_minimized());

                auto w2_render_rect = w2.frame().render_rect();
                auto w2_render_rect_on_screen = w2_render_rect;
                auto w2_transition_offset = window_transition_offset(w2);
                if (window_stack_transition_in_progress)
                    w2_render_rect_on_screen.translate_by(w2_transition_offset);
                if (!render_rect_on_screen.intersects(w2_render_rect_on_screen))
                    return IterationDecision::Continue;

                auto opaque_rects = w2.frame().opaque_render_rects();
                auto transparent_rects = w2.frame().transparent_render_rects();
                if (window_stack_transition_in_progress) {
                    auto transition_offset_2 = window_transition_offset(w2);
                    opaque_rects.translate_by(transition_offset_2);
                    transparent_rects.translate_by(transition_offset_2);
                }
                opaque_rects = opaque_rects.intersected(render_rect_on_screen);
                transparent_rects = transparent_rects.intersected(render_rect_on_screen);
                if (opaque_rects.is_empty() && transparent_rects.is_empty())
                    return IterationDecision::Continue;
                // 这两个集合来自同一个窗口框，正常情况下不相交。真被破坏时
                // （例如主题圆角与颜色键透明的组合）只让这一帧少画一点透明区域，
                // 不能把整个 WindowServer 断言掉。
                if (opaque_rects.intersects(transparent_rects))
                    transparent_rects = transparent_rects.shatter(opaque_rects);
                for (auto& covering : opaque_rects.rects()) {
                    opaque_covering.add(covering);
                    if (!visible_window_rects.is_empty())
                        visible_window_rects = visible_window_rects.shatter(covering);
                    if (!visible_opaque.is_empty()) {
                        auto uncovered_opaque = visible_opaque.shatter(covering);
                        visible_opaque = move(uncovered_opaque);
                    }
                    if (!transparency_rects.is_empty()) {
                        auto uncovered_transparency = transparency_rects.shatter(covering);
                        transparency_rects = move(uncovered_transparency);
                    }
                    if (!transparent_covering.is_empty()) {
                        auto uncovered_transparency = transparent_covering.shatter(covering);
                        transparent_covering = move(uncovered_transparency);
                    }
                }
                if (!transparent_rects.is_empty())
                    transparent_covering.add(transparent_rects.shatter(opaque_covering));
                // 累积的不透明/透明覆盖区不该相交；相交时宁可少画一块透明区。
                if (transparent_covering.intersects(opaque_covering))
                    transparent_covering = transparent_covering.shatter(opaque_covering);
                return IterationDecision::Continue;
            });
            VERIFY(opaque_covering.is_empty() || render_rect_on_screen.contains(opaque_covering.rects()));
            if (!m_overlay_rects.is_empty() && m_overlay_rects.intersects(visible_opaque)) {
                // In order to render overlays flicker-free we need to force this area into the
                // temporary transparency rendering buffer
                transparent_covering.add(m_overlay_rects.intersected(visible_opaque));
            }
            if (!transparent_covering.is_empty()) {
                if (transparent_covering.intersects(opaque_covering))
                    transparent_covering = transparent_covering.shatter(opaque_covering);
            }
            if (!transparent_covering.is_empty()) {
                transparency_rects.add(transparent_covering);
                if (!visible_opaque.is_empty()) {
                    auto uncovered_opaque = visible_opaque.shatter(transparent_covering);
                    visible_opaque = move(uncovered_opaque);
                }

                // Now that we know what transparency rectangles are immediately covering our window
                // figure out what windows they belong to and add them to the affected transparency rects.
                // We can't do the same with the windows below as we haven't gotten to those yet. These
                // will be determined after we're done with this pass.
                found_this_window = false;
                wm.for_each_visible_window_from_back_to_front([&](Window& w2) {
                    if (!found_this_window) {
                        if (&w == &w2)
                            found_this_window = true;
                        return IterationDecision::Continue;
                    }

                    auto affected_transparency = transparent_covering.intersected(w2.transparency_rects());
                    if (!affected_transparency.is_empty()) {
                        // 同一窗口可能被多个透明区命中：合并即可，不能假设只插入一次。
                        affected_transparency_rects.ensure(&w2).add(affected_transparency);
                    }
                    return IterationDecision::Continue;
                });
            }

            // This window should not be occluded while the window switcher is interested in it (depending
            // on the mode it's in). If it isn't then determine occlusions based on whether the window
            // rect has any visible areas at all.
            w.set_occluded(never_occlude(w.window_stack()) ? false : visible_window_rects.is_empty());

            bool have_opaque = !visible_opaque.is_empty();
            if (!transparency_rects.is_empty())
                have_transparent = true;
            if (have_transparent || have_opaque) {
                // Figure out what screens this window is rendered on
                // We gather this information so we can more quickly
                // render the window on each of the screens that it
                // needs to be rendered on.
                Screen::for_each([&](auto& screen) {
                    auto screen_rect = screen.rect();
                    for (auto& r : visible_opaque.rects()) {
                        if (r.intersects(screen_rect)) {
                            w.screens().append(&screen);
                            return IterationDecision::Continue;
                        }
                    }
                    for (auto& r : transparency_rects.rects()) {
                        if (r.intersects(screen_rect)) {
                            w.screens().append(&screen);
                            return IterationDecision::Continue;
                        }
                    }
                    return IterationDecision::Continue;
                });
            }

            if (!visible_opaque.is_empty()) {
                VERIFY(!visible_opaque.intersects(transparency_rects));

                // Determine visible area for the window below
                remaining_visible_screen_rects = remaining_visible_screen_rects.shatter(visible_opaque);
            }
            return IterationDecision::Continue;
        });

        if (have_transparent) {
            // Also, now that we have completed the first pass we can determine the affected
            // transparency rects below a given window
            wm.for_each_visible_window_from_back_to_front([&](Window& w) {
                // Any area left in remaining_visible_screen_rects will need to be rendered with the wallpaper first
                auto& transparency_rects = w.transparency_rects();
                auto& transparency_wallpaper_rects = w.transparency_wallpaper_rects();
                if (transparency_rects.is_empty()) {
                    VERIFY(transparency_wallpaper_rects.is_empty()); // Should have been cleared in the first pass
                } else {
                    transparency_wallpaper_rects = remaining_visible_screen_rects.intersected(transparency_rects);

                    if (!transparency_wallpaper_rects.is_empty()) {
                        auto remaining_visible = remaining_visible_screen_rects.shatter(transparency_wallpaper_rects);
                        remaining_visible_screen_rects = move(remaining_visible);
                    }
                }

                // Figure out the affected transparency rects underneath. First figure out if any transparency is visible at all
                Gfx::DisjointIntRectSet transparent_underneath;
                wm.for_each_visible_window_from_back_to_front([&](Window& w2) {
                    if (&w == &w2)
                        return IterationDecision::Break;
                    auto& opaque_rects2 = w2.opaque_rects();
                    if (!opaque_rects2.is_empty()) {
                        auto uncovered_transparency = transparent_underneath.shatter(opaque_rects2);
                        transparent_underneath = move(uncovered_transparency);
                    }
                    w2.transparency_rects().for_each_intersected(transparency_rects, [&](auto& rect) {
                        transparent_underneath.add(rect);
                        return IterationDecision::Continue;
                    });

                    return IterationDecision::Continue;
                });
                if (!transparent_underneath.is_empty()) {
                    // Now that we know there are some transparency rects underneath that are visible
                    // figure out what windows they belong to
                    auto& affected_transparency_rects = w.affected_transparency_rects();
                    wm.for_each_visible_window_from_back_to_front([&](Window& w2) {
                        if (&w == &w2)
                            return IterationDecision::Break;
                        auto& transparency_rects2 = w2.transparency_rects();
                        if (transparency_rects2.is_empty())
                            return IterationDecision::Continue;

                        auto affected_transparency = transparent_underneath.intersected(transparency_rects2);
                        if (!affected_transparency.is_empty()) {
                            affected_transparency_rects.ensure(&w2).add(affected_transparency);
                        }
                        return IterationDecision::Continue;
                    });
                }
                return IterationDecision::Continue;
            });
        }

        m_transparent_wallpaper_rects.clear_with_capacity();
        if (!m_overlay_rects.is_empty() && m_overlay_rects.intersects(remaining_visible_screen_rects)) {
            // Check if any overlay rects are remaining that are not somehow above any windows
            m_transparent_wallpaper_rects = m_overlay_rects.intersected(remaining_visible_screen_rects);
            auto remaining_visible_not_covered = remaining_visible_screen_rects.shatter(m_overlay_rects);
            remaining_visible_screen_rects = move(remaining_visible_not_covered);
        }

        m_opaque_wallpaper_rects = move(remaining_visible_screen_rects);
    }

    if constexpr (OCCLUSIONS_DEBUG) {
        for (auto& r : m_opaque_wallpaper_rects.rects())
            dbgln("  wallpaper opaque: {}", r);
    }

    wm.for_each_visible_window_from_back_to_front([&](Window& w) {
        auto window_frame_rect = w.frame().render_rect();
        if (w.is_minimized() || window_frame_rect.is_empty() || w.screens().is_empty())
            return IterationDecision::Continue;

        if constexpr (OCCLUSIONS_DEBUG) {
            dbgln("  Window {} frame rect: {} rendered on screens: {}", w.title(), window_frame_rect, w.screens().size());
            for (auto& s : w.screens())
                dbgln("    screen: #{}", s->index());
            for (auto& r : w.opaque_rects().rects())
                dbgln("    opaque: {}", r);
            for (auto& r : w.transparency_wallpaper_rects().rects())
                dbgln("    transparent wallpaper: {}", r);
            for (auto& r : w.transparency_rects().rects())
                dbgln("    transparent: {}", r);
            for (auto& it : w.affected_transparency_rects()) {
                dbgln("    affects {}:", it.key->title());
                for (auto& r : it.value.rects())
                    dbgln("        transparent: {}", r);
            }
        }

        // 缓存的不透明/透明区与壁纸可见区本应互不相交；真的相交只意味着多画
        // 一点壁纸（视觉问题），不值得让整个服务器 abort。开调试时打一行日志。
        if constexpr (OCCLUSIONS_DEBUG) {
            if (w.opaque_rects().intersects(m_opaque_wallpaper_rects)
                || w.transparency_rects().intersects(m_opaque_wallpaper_rects)
                || w.transparency_wallpaper_rects().intersects(m_opaque_wallpaper_rects))
                dbgln("  warning: window '{}' cached rects intersect opaque wallpaper rects", w.title());
        }
        return IterationDecision::Continue;
    });
}

void Compositor::register_animation(Badge<Animation>, Animation& animation)
{
    VERIFY(!m_animations_running);
    bool was_empty = m_animations.is_empty();
    auto result = m_animations.set(&animation);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    if (was_empty) {
        m_invalidated_any = true;
        start_compose_async_timer();
    }
}

void Compositor::unregister_animation(Badge<Animation>, Animation& animation)
{
    VERIFY(!m_animations_running);
    bool was_removed = m_animations.remove(&animation);
    VERIFY(was_removed);
}

void Compositor::update_animations(Screen& screen, Gfx::DisjointIntRectSet& flush_rects)
{
    Vector<NonnullRefPtr<Animation>, 16> finished_animations;
    ScopeGuard call_stop_handlers([&] {
        for (auto& animation : finished_animations)
            animation->call_stop_handler({});
    });

    TemporaryChange animations_running(m_animations_running, true);
    auto& painter = *screen.compositor_screen_data().m_back_painter;
    // Iterating over the animations using remove_all_matching we can iterate
    // and immediately remove finished animations without having to keep track
    // of them in a separate container.
    m_animations.remove_all_matching([&](auto* animation) {
        VERIFY(animation->is_running());
        if (!animation->update(painter, screen, flush_rects)) {
            // Mark it as removed so that the Animation::on_stop handler doesn't
            // trigger the Animation object from being destroyed, causing it to
            // unregister while we still loop over them.
            animation->was_removed({});

            finished_animations.append(*animation);
            return true;
        }
        return false;
    });
}

void Compositor::create_window_stack_switch_overlay(WindowStack& target_stack)
{
    stop_window_stack_switch_overlay_timer();
    Screen::for_each([&](auto& screen) {
        auto& screen_data = screen.compositor_screen_data();
        screen_data.m_window_stack_switch_overlay = nullptr; // delete it first
        screen_data.m_window_stack_switch_overlay = create_overlay<WindowStackSwitchOverlay>(screen, target_stack);
        screen_data.m_window_stack_switch_overlay->set_enabled(true);
        return IterationDecision::Continue;
    });
}

void Compositor::remove_window_stack_switch_overlays()
{
    Screen::for_each([&](auto& screen) {
        screen.compositor_screen_data().m_window_stack_switch_overlay = nullptr;
        return IterationDecision::Continue;
    });
}

void Compositor::stop_window_stack_switch_overlay_timer()
{
    if (m_stack_switch_overlay_timer) {
        // Cancel any timer, we're going to delete the overlay
        m_stack_switch_overlay_timer->stop();
        m_stack_switch_overlay_timer = nullptr;
    }
}

void Compositor::start_window_stack_switch_overlay_timer()
{
    if (m_stack_switch_overlay_timer) {
        m_stack_switch_overlay_timer->stop();
        m_stack_switch_overlay_timer = nullptr;
    }
    bool have_overlay = false;
    Screen::for_each([&](auto& screen) {
        if (screen.compositor_screen_data().m_window_stack_switch_overlay) {
            have_overlay = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!have_overlay)
        return;
    m_stack_switch_overlay_timer = Core::Timer::create_single_shot(
        500,
        [this] {
            remove_window_stack_switch_overlays();
        },
        this);
    m_stack_switch_overlay_timer->start();
}

void Compositor::finish_window_stack_switch()
{
    VERIFY(m_transitioning_to_window_stack);
    VERIFY(m_current_window_stack);
    VERIFY(m_transitioning_to_window_stack != m_current_window_stack);

    m_current_window_stack->set_transition_offset({}, {});
    m_transitioning_to_window_stack->set_transition_offset({}, {});

    auto* previous_window_stack = m_current_window_stack;
    m_current_window_stack = m_transitioning_to_window_stack;
    m_transitioning_to_window_stack = nullptr;

    m_window_stack_transition_animation = nullptr;

    auto& wm = WindowManager::the();
    if (!wm.m_switcher->is_visible())
        previous_window_stack->set_all_occluded(true);
    wm.did_switch_window_stack({}, *previous_window_stack, *m_current_window_stack);

    invalidate_occlusions();

    // Rather than invalidating the entire we could invalidate all render rectangles
    // that are affected by the transition offset before and after changing it.
    invalidate_screen();

    start_window_stack_switch_overlay_timer();
}

void Compositor::set_current_window_stack_no_transition(WindowStack& new_window_stack)
{
    if (m_transitioning_to_window_stack) {
        finish_window_stack_switch();
        VERIFY(!m_window_stack_transition_animation);
        VERIFY(!m_transitioning_to_window_stack);
    }
    if (m_current_window_stack == &new_window_stack)
        return;
    m_current_window_stack = &new_window_stack;
    invalidate_for_window_stack_merge_or_change();
}

void Compositor::invalidate_for_window_stack_merge_or_change()
{
    invalidate_occlusions();
    invalidate_screen();
}

void Compositor::switch_to_window_stack(WindowStack& new_window_stack, bool show_overlay)
{
    if (m_transitioning_to_window_stack) {
        if (m_transitioning_to_window_stack == &new_window_stack)
            return;
        // A switch is in progress, but the user is impatient. Finish the transition instantly
        finish_window_stack_switch();
        VERIFY(!m_window_stack_transition_animation);
        // Now switch to the next target as usual
    }
    VERIFY(m_current_window_stack);

    if (&new_window_stack == m_current_window_stack) {
        // So that the user knows which stack they're on, show the overlay briefly
        if (show_overlay) {
            create_window_stack_switch_overlay(*m_current_window_stack);
            start_window_stack_switch_overlay_timer();
        } else {
            stop_window_stack_switch_overlay_timer();
            remove_window_stack_switch_overlays();
        }
        return;
    }
    VERIFY(!m_transitioning_to_window_stack);
    m_transitioning_to_window_stack = &new_window_stack;

    auto window_stack_size = Screen::bounding_rect().size();

    int delta_x = 0;
    if (new_window_stack.column() < m_current_window_stack->column())
        delta_x = window_stack_size.width();
    else if (new_window_stack.column() > m_current_window_stack->column())
        delta_x = -window_stack_size.width();
    int delta_y = 0;
    if (new_window_stack.row() < m_current_window_stack->row())
        delta_y = window_stack_size.height();
    else if (new_window_stack.row() > m_current_window_stack->row()) {
        delta_y = -window_stack_size.height();
    }

    m_transitioning_to_window_stack->set_transition_offset({}, { -delta_x, -delta_y });
    m_current_window_stack->set_transition_offset({}, {});

    if (show_overlay) {
        // We start the timer when the animation ends!
        create_window_stack_switch_overlay(*m_transitioning_to_window_stack);
    } else {
        stop_window_stack_switch_overlay_timer();
        remove_window_stack_switch_overlays();
    }

    VERIFY(!m_window_stack_transition_animation);
    m_window_stack_transition_animation = Animation::create();
    m_window_stack_transition_animation->set_duration(250);
    m_window_stack_transition_animation->on_update = [this, delta_x, delta_y](float progress, Gfx::Painter&, Screen&, Gfx::DisjointIntRectSet&) {
        VERIFY(m_transitioning_to_window_stack);
        VERIFY(m_current_window_stack);

        // Set transition offset for the window stack we're transitioning out of
        auto previous_transition_offset_from = m_current_window_stack->transition_offset();
        Gfx::IntPoint transition_offset_from { (float)delta_x * progress, (float)delta_y * progress };
        if (previous_transition_offset_from == transition_offset_from)
            return;

        {
            // we need to render both, the existing dirty rectangles as well as where we're shifting to
            auto translated_dirty_rects = m_dirty_screen_rects.clone();
            auto transition_delta = transition_offset_from - previous_transition_offset_from;
            translated_dirty_rects.translate_by(transition_delta);
            m_dirty_screen_rects.add(translated_dirty_rects.intersected(Screen::bounding_rect()));
        }
        m_current_window_stack->set_transition_offset({}, transition_offset_from);

        // Set transition offset for the window stack we're transitioning to
        Gfx::IntPoint transition_offset_to { (float)-delta_x * (1.0f - progress), (float)-delta_y * (1.0f - progress) };
        m_transitioning_to_window_stack->set_transition_offset({}, transition_offset_to);

        invalidate_occlusions();

        // Rather than invalidating the entire we could invalidate all render rectangles
        // that are affected by the transition offset before and after changing it.
        invalidate_screen();
    };

    m_window_stack_transition_animation->on_stop = [this] {
        finish_window_stack_switch();
    };
    m_window_stack_transition_animation->start();
}

}
