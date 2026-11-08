/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SkinCanvas.h"
#include "SkinTheme.h"
#include "TTSkin.h"
#include <AK/Function.h>
#include <LibGUI/Event.h>
#include <LibGUI/Widget.h>

// Backdrop shared by the 千千静听 windows: paints the active skin's bitmap for
// this window when one is available, otherwise the built-in gradient.
class SkinPanel final : public GUI::Widget {
    C_OBJECT(SkinPanel)

public:
    virtual ~SkinPanel() override = default;

    void set_kind(SkinTheme::WindowKind kind)
    {
        m_kind = kind;
        update();
    }

    // Frameless windows paint their own chrome: a slim title strip with a
    // close button, and the whole background acts as a drag handle.
    void set_frameless_style(bool frameless)
    {
        m_frameless_style = frameless;
        update();
    }

    Function<void()> on_close_clicked;

    // Frameless windows have no system menu bar, so the player exposes its own
    // main menu from a right-click anywhere on the skin. ContextMenuEvent
    // bubbles up through the widget tree, which makes the root panel the one
    // place that needs to handle it.
    Function<void(GUI::ContextMenuEvent&)> on_context_menu_request;

    // Native Skin.xml mode: SkinPanel hides its LibGUI children and delegates
    // all painting/hit-testing to a SkinCanvas. These hooks let each window
    // bind the skin's control names to its own playback logic.
    Function<void(StringView name)> on_native_action;
    Function<void(StringView name, float ratio)> on_native_slider;
    Function<void(SkinCanvas&)> on_native_canvas_setup;

    // Call after a skin switch so the canvas re-reads the new document even
    // when the selected index happens to be unchanged.
    void refresh_native_layout();

    // Native skins only draw the chrome; the list contents (playlist rows,
    // lyrics) are the app's job. Registering a content widget keeps exactly
    // that one child visible and stretches it over the named skin element's
    // list box instead of hiding it with the rest of the LibGUI tree.
    enum class ContentFill {
        // List boxes (`playlist`, `lyric`): the element marks the top-left
        // corner and width, the box runs to the panel's bottom edge.
        ExtendToPanel,
        // Fixed frames (`visual`): the element is the whole widget.
        ExactBounds,
    };
    void set_content_widget(GUI::Widget&, StringView element_name, ContentFill = ContentFill::ExtendToPanel,
        bool part_of_classic_layout = true);

    // Live text/value updates from the window's playback state. May be null
    // before the first paint (the canvas is created lazily).
    SkinCanvas* native_canvas() { return m_canvas.has_value() ? &m_canvas.value() : nullptr; }

    virtual void paint_event(GUI::PaintEvent& event) override;
    // Runs after the panel's own layout, so it can put the content view back
    // where the skin wants it (the BoxLayout would stretch it over the whole
    // panel as soon as it becomes the only visible child).
    virtual void did_layout() override;
    virtual void mousedown_event(GUI::MouseEvent& event) override;
    virtual void mouseup_event(GUI::MouseEvent& event) override;
    virtual void mousemove_event(GUI::MouseEvent& event) override;
    virtual void leave_event(Core::Event& event) override;
    virtual void context_menu_event(GUI::ContextMenuEvent& event) override;

private:
    SkinPanel()
    {
        set_fill_with_background_color(true);
    }

    Gfx::IntRect close_button_rect() const;
    void paint_title_strip(GUI::Painter&);
    Gfx::Bitmap const* scaled_background(Gfx::Bitmap const* background, Gfx::IntSize size);
    void sync_native_layout();
    void set_children_visible(bool visible);
    void layout_content_widget();

    SkinTheme::WindowKind m_kind { SkinTheme::WindowKind::Main };
    Optional<SkinCanvas> m_canvas;
    int m_canvas_skin { -2 };
    bool m_children_hidden {};
    bool m_frameless_style { true };

    GUI::Widget* m_content_widget { nullptr };
    ByteString m_content_element;
    Gfx::IntRect m_content_rect;
    ContentFill m_content_fill { ContentFill::ExtendToPanel };
    bool m_content_in_classic_layout { true };

    // Skin bitmaps have a fixed size. Windows can be restored at any size, so
    // keep one scaled copy per panel instead of blitting 1:1 and leaving an
    // unpainted band at the right/bottom.
    RefPtr<Gfx::Bitmap> m_scaled_background;
    Gfx::IntSize m_scaled_size;
    int m_scaled_skin { -2 };
};
