/*
 * serenity-paint-bridge — thin C-ABI shim over Serenity's REAL painter.
 *
 * GTK must not reimplement ClassicStylePainter (that was the cairo copy in
 * serenity-widgets.c, now deleted). Instead we call the actual
 * Gfx::StylePainter from liblagom-gfx, painting into a Gfx::Bitmap and
 * handing the pixels back as a plain ARGB32 buffer that libgtk blits into a
 * cairo surface. Serenity (WindowServer/compositor) is never involved: this
 * runs entirely in the app process via the Lagom host build of LibGfx.
 *
 * The palette is not hardcoded: it is built from libserenity-ui, i.e. from
 * the very same WindowServer.ini / <Theme>.ini the native desktop reads, so
 * a GTK button and a GUI::Button resolve to identical colours and a theme
 * switch is picked up on the next paint (serenity_ui_revision()).
 *
 * Everything is extern "C" so libgtk stays a pure-C consumer and only needs
 * dlopen/dlsym — no AK/LibGfx/ABI leaks into libgtk's link or export table.
 */

#include <AK/Error.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Orientation.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/TabPosition.h>
#include <LibGfx/SystemTheme.h>

#include "serenity-ui.h"

#include <string.h>

using namespace Gfx;

namespace {

u32 argb_of(SerenityColor c)
{
    auto clamp255 = [](double v) { return (u32)(v < 0 ? 0 : (v > 1 ? 255 : v * 255.0 + 0.5)); };
    return (clamp255(c.a) << 24) | (clamp255(c.r) << 16) | (clamp255(c.g) << 8) | clamp255(c.b);
}

Palette* current_palette()
{
    static Palette* cached = nullptr;
    static u64 cached_revision = ~0ull;

    u64 revision = serenity_ui_revision();
    if (cached && revision == cached_revision)
        return cached;
    cached_revision = revision;

    auto buf_or = Core::AnonymousBuffer::create_with_size(sizeof(SystemTheme));
    if (buf_or.is_error())
        return cached;
    auto buf = buf_or.release_value();

    auto* t = buf.data<SystemTheme>();
    memset(t, 0, sizeof(*t));

    auto set = [&](ColorRole r, SerenityRole sr) { t->color[(int)r] = argb_of(serenity_ui_color(sr)); };
    set(ColorRole::Window, SERENITY_ROLE_WINDOW);
    set(ColorRole::WindowText, SERENITY_ROLE_WINDOW_TEXT);
    set(ColorRole::Button, SERENITY_ROLE_FACE);
    set(ColorRole::ButtonText, SERENITY_ROLE_FACE_TEXT);
    set(ColorRole::Base, SERENITY_ROLE_BASE);
    set(ColorRole::BaseText, SERENITY_ROLE_BASE_TEXT);
    set(ColorRole::ThreedHighlight, SERENITY_ROLE_HIGHLIGHT);
    set(ColorRole::ThreedShadow1, SERENITY_ROLE_LIGHT);
    set(ColorRole::ThreedShadow2, SERENITY_ROLE_SHADOW);
    set(ColorRole::Selection, SERENITY_ROLE_SELECTION);
    set(ColorRole::SelectionText, SERENITY_ROLE_SELECTION_TEXT);
    set(ColorRole::InactiveSelection, SERENITY_ROLE_MENU_SELECTION);
    set(ColorRole::InactiveSelectionText, SERENITY_ROLE_MENU_SELECTION_TEXT);
    set(ColorRole::MenuBase, SERENITY_ROLE_MENU);
    set(ColorRole::MenuBaseText, SERENITY_ROLE_MENU_TEXT);
    set(ColorRole::MenuSelection, SERENITY_ROLE_MENU_SELECTION);
    set(ColorRole::MenuSelectionText, SERENITY_ROLE_MENU_SELECTION_TEXT);
    set(ColorRole::MenuStripe, SERENITY_ROLE_MENU_STRIPE);
    set(ColorRole::DisabledTextFront, SERENITY_ROLE_DISABLED_TEXT);
    set(ColorRole::DisabledTextBack, SERENITY_ROLE_DISABLED_TEXT_BACK);
    set(ColorRole::HoverHighlight, SERENITY_ROLE_HOVER);
    set(ColorRole::ActiveWindowBorder1, SERENITY_ROLE_TITLE_ACTIVE_1);
    set(ColorRole::ActiveWindowBorder2, SERENITY_ROLE_TITLE_ACTIVE_2);
    set(ColorRole::ActiveWindowTitle, SERENITY_ROLE_TITLE_TEXT);
    set(ColorRole::InactiveWindowBorder1, SERENITY_ROLE_TITLE_INACTIVE_1);
    set(ColorRole::InactiveWindowBorder2, SERENITY_ROLE_TITLE_INACTIVE_2);
    set(ColorRole::InactiveWindowTitle, SERENITY_ROLE_TITLE_INACTIVE_TEXT);
    set(ColorRole::Gutter, SERENITY_ROLE_GUTTER);
    set(ColorRole::GutterBorder, SERENITY_ROLE_SHADOW);
    set(ColorRole::FocusOutline, SERENITY_ROLE_FOCUS_OUTLINE);
    set(ColorRole::Link, SERENITY_ROLE_LINK);
    set(ColorRole::PlaceholderText, SERENITY_ROLE_PLACEHOLDER);

    t->metric[(int)MetricRole::BorderThickness] = serenity_ui_metric(SERENITY_METRIC_BORDER);
    t->metric[(int)MetricRole::TitleHeight] = serenity_ui_metric(SERENITY_METRIC_TITLEBAR_H);

    cached = new Palette(PaletteImpl::create_with_anonymous_buffer(buf));
    return cached;
}

int blit_bitmap(Gfx::Bitmap& bm, int w, int h, unsigned char* out)
{
    for (int y = 0; y < h; ++y)
        memcpy(out + (size_t)y * (size_t)w * 4, bm.scanline(y), (size_t)w * 4);
    return 0;
}

// Left stripe of a classic menu. 24px is what WindowServer::Menu uses; very
// narrow GTK popovers get a thinner strip so the text keeps its room.
int menu_stripe_width(int w)
{
    if (w >= 160)
        return 24;
    if (w >= 96)
        return 12;
    if (w >= 56)
        return 6;
    return 0;
}

}

extern "C" {

/* Paint one widget kind (see serenity-ui.h) into caller memory as tight
 * BGRA8888 (w*h*4 bytes). `state` is a SerenityWidgetState bitmask.
 * Returns 0 on success, negative on failure. */
int serenity_paint_widget(int w, int h, unsigned char* out,
                          int kind, int state, int vertical,
                          double value, int max_value, int arrow)
{
    if (w <= 0 || h <= 0 || !out)
        return -1;

    Palette* pal = current_palette();
    if (!pal)
        return -3;

    auto bm_or = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { w, h }, 1, {});
    if (bm_or.is_error())
        return -2;
    NonnullRefPtr<Gfx::Bitmap> bm = bm_or.release_value();

    IntRect rect(0, 0, w, h);
    Painter painter(*bm);
    auto& p = *pal;

    bool pressed = (state & SERENITY_STATE_PRESSED) != 0;
    bool hovered = (state & SERENITY_STATE_HOVER) != 0;
    bool checked = (state & SERENITY_STATE_CHECKED) != 0
                   || (state & SERENITY_STATE_SELECTED) != 0;
    bool enabled = (state & SERENITY_STATE_DISABLED) == 0;
    bool focused = (state & SERENITY_STATE_FOCUSED) != 0;
    bool is_default = (state & SERENITY_STATE_DEFAULT) != 0;

    switch ((SerenityWidgetKind)kind) {
    case SERENITY_WIDGET_BUTTON:
    case SERENITY_WIDGET_TOGGLE_BUTTON:
    case SERENITY_WIDGET_COMBO:
    case SERENITY_WIDGET_LIST_HEADER:
    case SERENITY_WIDGET_SCROLLBAR_BUTTON:
        StylePainter::paint_button(painter, rect, p, ButtonStyle::Normal,
                                   pressed, hovered, checked, enabled, focused, is_default);
        /* combo boxes and scrollbar steppers carry a small arrow */
        if (arrow != SERENITY_ARROW_NONE) {
            int cx = w / 2, cy = h / 2;
            IntPoint p1, p2, p3;
            switch ((SerenityArrow)arrow) {
            case SERENITY_ARROW_UP:
                p1 = { cx - 3, cy + 2 }; p2 = { cx, cy - 2 }; p3 = { cx + 3, cy + 2 }; break;
            case SERENITY_ARROW_DOWN:
                p1 = { cx - 3, cy - 2 }; p2 = { cx, cy + 2 }; p3 = { cx + 3, cy - 2 }; break;
            case SERENITY_ARROW_LEFT:
                p1 = { cx + 2, cy - 3 }; p2 = { cx - 2, cy }; p3 = { cx + 2, cy + 3 }; break;
            default:
                p1 = { cx - 2, cy - 3 }; p2 = { cx + 2, cy }; p3 = { cx - 2, cy + 3 }; break;
            }
            painter.draw_triangle(p1, p2, p3,
                                  enabled ? p.button_text() : p.threed_shadow2());
        }
        break;

    /* Scrollbar scrubber / slider: Serenity uses the thick-capped variant. */
    case SERENITY_WIDGET_SCROLLBAR_THUMB:
        StylePainter::paint_button(painter, rect, p, ButtonStyle::ThickCap,
                                   pressed, hovered, false, enabled, false, false);
        break;

    case SERENITY_WIDGET_ENTRY:
        painter.fill_rect(rect, enabled ? p.color(ColorRole::Base) : p.color(ColorRole::Window));
        StylePainter::paint_frame(painter, rect, p, FrameStyle::SunkenBox);
        break;

    case SERENITY_WIDGET_FRAME_RAISED:
    case SERENITY_WIDGET_GROUP_BOX:
        painter.fill_rect(rect, p.color(ColorRole::Button));
        StylePainter::paint_frame(painter, rect, p, FrameStyle::RaisedBox);
        break;

    case SERENITY_WIDGET_FRAME_SUNKEN:
        painter.fill_rect(rect, p.color(ColorRole::Base));
        StylePainter::paint_frame(painter, rect, p, FrameStyle::SunkenBox);
        break;

    /* Scrollbar gutter: the dithered "no content here" pattern. */
    case SERENITY_WIDGET_SCROLLBAR_TROUGH:
        painter.fill_rect_with_dither_pattern(rect, p.button().lightened(1.3f), p.button());
        break;

    case SERENITY_WIDGET_CHECK:
        StylePainter::paint_check_box(painter, rect, p, enabled, checked, pressed);
        break;

    case SERENITY_WIDGET_RADIO:
        StylePainter::paint_radio_button(painter, rect, p, checked, pressed);
        break;

    /* Menu panel: hard black outline, menu base inset by 2px, left stripe
     * (WindowServer::Menu::draw does exactly this). */
    case SERENITY_WIDGET_MENU: {
        painter.fill_rect(rect, Color::White);
        painter.draw_rect(rect, Color::Black);
        painter.fill_rect(rect.shrunken(2, 2), p.menu_base());
        int stripe = menu_stripe_width(w);
        if (stripe > 0)
            painter.fill_rect({ 1, 1, stripe + 1, h - 2 }, p.menu_stripe());
        painter.draw_rect(rect.shrunken(1, 1), p.threed_shadow1());
        break;
    }

    case SERENITY_WIDGET_MENU_ITEM: {
        painter.fill_rect(rect, p.menu_base());
        int stripe = menu_stripe_width(w);
        if (stripe > 0)
            painter.fill_rect({ 0, 0, stripe, h }, p.menu_stripe());
        if (hovered || checked) {
            IntRect item = rect;
            if (stripe > 0)
                item.set_left(stripe);
            painter.fill_rect(item, p.menu_selection());
            painter.draw_rect(item, p.menu_selection().darkened());
        }
        break;
    }

    /* Hover/open highlight of a single menu row, painted over the menu
     * panel's own base+stripe (the idle row is left transparent so those
     * stay pixel-aligned). Same shape WindowServer::Menu::draw_highlight. */
    case SERENITY_WIDGET_MENU_SELECTION: {
        IntRect item = rect;
        int stripe = menu_stripe_width(w);
        if (stripe > 0 && w > stripe + 4)
            item.set_left(stripe);
        painter.fill_rect(item, p.menu_selection());
        painter.draw_rect(item, p.menu_selection().darkened());
        break;
    }

    case SERENITY_WIDGET_TAB:
    case SERENITY_WIDGET_TAB_ACTIVE:
        /* GTK hands us the tab's label node (16px tall), too short for the
         * classic tab outline, which assumes a full notebook header. At that
         * size a button-style bevel reads correctly as a GTK2 tab, with the
         * selected one sunk into the page. */
        if (h < 20) {
            StylePainter::paint_button(painter, rect, p, ButtonStyle::Normal,
                                       checked, hovered, false, enabled, false, false);
            break;
        }
        StylePainter::paint_tab_button(painter, rect, p,
                                       checked || (SerenityWidgetKind)kind == SERENITY_WIDGET_TAB_ACTIVE,
                                       hovered, enabled,
                                       TabPosition::Top, true, false);
        break;

    /* One call paints the whole progress bar: gradient bar + the carved-out
     * base-coloured remainder (ClassicStylePainter::paint_progressbar). */
    case SERENITY_WIDGET_PROGRESS_TROUGH:
    case SERENITY_WIDGET_PROGRESS_FILL: {
        int max = max_value > 0 ? max_value : 100;
        int val = (int)(value * (double)max + 0.5);
        StylePainter::paint_progressbar(painter, rect, p, 0, max, val, {},
                                        vertical ? Orientation::Vertical : Orientation::Horizontal);
        break;
    }

    /* Bars are flat: 1px highlight on top, 1px shadow at the bottom. */
    case SERENITY_WIDGET_MENU_BAR:
    case SERENITY_WIDGET_TOOLBAR:
    case SERENITY_WIDGET_HEADER_BAR:
    case SERENITY_WIDGET_STATUS_BAR:
        painter.fill_rect(rect, p.button());
        painter.draw_line({ 0, 0 }, { w - 1, 0 }, p.threed_highlight());
        painter.draw_line({ 0, h - 1 }, { w - 1, h - 1 }, p.threed_shadow1());
        break;

    /* Splitter grip: classic 3D dots. */
    case SERENITY_WIDGET_HANDLE:
        painter.fill_rect(rect, p.button());
        for (int x = 2; x + 3 < w; x += 4) {
            painter.fill_rect({ x, 2, 2, 2 }, p.threed_highlight());
            painter.fill_rect({ x + 1, 3, 2, 2 }, p.threed_shadow1());
        }
        break;

    case SERENITY_WIDGET_SEPARATOR:
        if (vertical) {
            painter.draw_line({ w / 2, 0 }, { w / 2, h - 1 }, p.threed_shadow1());
            painter.draw_line({ w / 2 + 1, 0 }, { w / 2 + 1, h - 1 }, p.threed_highlight());
        } else {
            painter.draw_line({ 0, h / 2 }, { w - 1, h / 2 }, p.threed_shadow1());
            painter.draw_line({ 0, h / 2 + 1 }, { w - 1, h / 2 + 1 }, p.threed_highlight());
        }
        break;

    default:
        painter.fill_rect(rect, p.button());
        break;
    }

    (void)arrow;
    return blit_bitmap(*bm, w, h, out);
}

/* A whole classic scrollbar: dithered gutter plus a thick-capped scrubber.
 * GTK4 never hands us the slider as a widget, so the geometry is computed by
 * the caller from the GtkAdjustment and handed in. */
int serenity_paint_scrollbar(int w, int h, unsigned char* out, int vertical,
                             int thumb_start, int thumb_len, int pressed, int hovered)
{
    if (w <= 0 || h <= 0 || !out)
        return -1;
    Palette* pal = current_palette();
    if (!pal)
        return -3;
    auto bm_or = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { w, h }, 1, {});
    if (bm_or.is_error())
        return -2;
    NonnullRefPtr<Gfx::Bitmap> bm = bm_or.release_value();

    IntRect rect(0, 0, w, h);
    Painter painter(*bm);
    auto& p = *pal;

    painter.fill_rect_with_dither_pattern(rect, p.button().lightened(1.3f), p.button());

    int track = vertical ? h : w;
    if (thumb_len > 0 && thumb_len <= track) {
        if (thumb_start < 0) thumb_start = 0;
        if (thumb_start + thumb_len > track) thumb_start = track - thumb_len;
        IntRect thumb = vertical ? IntRect(0, thumb_start, w, thumb_len)
                                 : IntRect(thumb_start, 0, thumb_len, h);
        StylePainter::paint_button(painter, thumb, p, ButtonStyle::ThickCap,
                                   pressed != 0, hovered != 0, false, true, false, false);
    }
    return blit_bitmap(*bm, w, h, out);
}

/* Paint only a scroll/combo arrow glyph (transparent elsewhere). Used by GTK3,
 * whose gtk_render_arrow() draws the stepper arrow separately from the
 * stepper face. This is a triangle drawn with Painter primitives, not a
 * second implementation of the widget look. */
int serenity_paint_arrow(int w, int h, unsigned char* out, int arrow, int enabled)
{
    if (w <= 0 || h <= 0 || !out)
        return -1;
    Palette* pal = current_palette();
    if (!pal)
        return -3;
    auto bm_or = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { w, h }, 1, {});
    if (bm_or.is_error())
        return -2;
    NonnullRefPtr<Gfx::Bitmap> bm = bm_or.release_value();

    IntRect rect(0, 0, w, h);
    Painter painter(*bm);
    painter.clear_rect(rect, Gfx::Color::Transparent);

    // Same 7x4 arrow the classic scrollbar steppers use.
    int cx = w / 2;
    int cy = h / 2;
    IntPoint p1, p2, p3;
    switch ((SerenityArrow)arrow) {
    case SERENITY_ARROW_UP:
        p1 = { cx - 3, cy + 2 }; p2 = { cx, cy - 2 }; p3 = { cx + 3, cy + 2 };
        break;
    case SERENITY_ARROW_DOWN:
        p1 = { cx - 3, cy - 2 }; p2 = { cx, cy + 2 }; p3 = { cx + 3, cy - 2 };
        break;
    case SERENITY_ARROW_LEFT:
        p1 = { cx + 2, cy - 3 }; p2 = { cx - 2, cy }; p3 = { cx + 2, cy + 3 };
        break;
    case SERENITY_ARROW_RIGHT:
        p1 = { cx - 2, cy - 3 }; p2 = { cx + 2, cy }; p3 = { cx - 2, cy + 3 };
        break;
    default:
        return blit_bitmap(*bm, w, h, out);
    }
    painter.draw_triangle(p1, p2, p3, enabled ? pal->button_text() : pal->threed_shadow2());
    return blit_bitmap(*bm, w, h, out);
}

/* Report the palette the bridge is painting with, so a caller can verify it
 * matches what the desktop itself is using. */
int serenity_paint_palette_hex(int role, char* out, int out_size)
{
    if (!out || out_size < 8 || role < 0 || role >= SERENITY_ROLE_COUNT)
        return -1;
    serenity_ui_color_hex((SerenityRole)role, out);
    return 0;
}

} // extern "C"
