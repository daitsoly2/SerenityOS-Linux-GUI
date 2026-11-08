/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinCanvas.h"
#include <AK/Format.h>
#include <LibGUI/Painter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/TextAlignment.h>

namespace {

// Native `align` packs horizontal (low nibble) and vertical (high nibble):
// left=1 / center=2 / right=3, top=0x10 / vcenter=0x20 / bottom=0x30.
Gfx::TextAlignment to_text_alignment(unsigned alignment)
{
    unsigned horizontal = alignment & 0x0fU;
    unsigned vertical = alignment & 0xf0U;
    if (vertical == 0x10U) {
        if (horizontal == 1) return Gfx::TextAlignment::TopLeft;
        if (horizontal == 3) return Gfx::TextAlignment::TopRight;
        return Gfx::TextAlignment::TopCenter;
    }
    if (vertical == 0x30U) {
        if (horizontal == 1) return Gfx::TextAlignment::BottomLeft;
        if (horizontal == 3) return Gfx::TextAlignment::BottomRight;
        return Gfx::TextAlignment::BottomCenter;
    }
    if (horizontal == 1) return Gfx::TextAlignment::CenterLeft;
    if (horizontal == 3) return Gfx::TextAlignment::CenterRight;
    return Gfx::TextAlignment::Center;
}

float clamp_ratio(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

// Native control bitmaps paint their background in the skin's mask colour (a
// round button sits on a magenta square). Those pixels have to be skipped so
// the panel underneath shows through: letting them reach the backing store
// made the window-level colour key punch a hole to the desktop, which is why
// every round button had a dark square behind it.
void blit_skipping_key(GUI::Painter& painter, Gfx::IntPoint destination, Gfx::Bitmap const& source,
    Gfx::IntRect const& source_rect, Optional<Gfx::Color> const& key)
{
    if (source_rect.is_empty())
        return;
    if (!key.has_value()) {
        painter.blit(destination, source, source_rect);
        return;
    }
    auto near = [](u8 a, u8 b) { return (a > b ? a - b : b - a) <= 16; };
    for (int y = 0; y < source_rect.height(); ++y) {
        for (int x = 0; x < source_rect.width(); ++x) {
            auto pixel = source.get_pixel(source_rect.x() + x, source_rect.y() + y);
            if (pixel.alpha() == 0)
                continue;
            if (near(pixel.red(), key->red()) && near(pixel.green(), key->green()) && near(pixel.blue(), key->blue()))
                continue;
            painter.set_pixel(destination.x() + x, destination.y() + y, pixel);
        }
    }
}

}

void SkinCanvas::invalidate()
{
    if (on_invalidate)
        on_invalidate();
}

void SkinCanvas::set_layout(SkinPanelLayout layout)
{
    m_layout = move(layout);
    invalidate();
}

void SkinCanvas::set_element_text(StringView name, ByteString text)
{
    m_texts.set(name.to_byte_string(), move(text));
    invalidate();
}

void SkinCanvas::set_element_value(StringView name, float ratio)
{
    m_values.set(name.to_byte_string(), clamp_ratio(ratio));
    invalidate();
}

void SkinCanvas::set_element_enabled(StringView name, bool enabled)
{
    m_disabled.set(name.to_byte_string(), !enabled);
    invalidate();
}

SkinElement const* SkinCanvas::hit_test(Gfx::IntPoint point) const
{
    if (!m_layout.has_value())
        return nullptr;
    // Front-to-back order: the first matching element wins, matching the
    // native HWND z-order hit test.
    for (auto const& element : m_layout->elements) {
        if (element.bounds.contains(point))
            return &element;
    }
    return nullptr;
}

SkinElement const* SkinCanvas::find(StringView name) const
{
    if (!m_layout.has_value())
        return nullptr;
    for (auto const& element : m_layout->elements) {
        if (element.name.view() == name)
            return &element;
    }
    return nullptr;
}

bool SkinCanvas::is_slider(SkinElement const& element) const
{
    if (element.bar_image || element.fill_image || element.fill_image2 || element.thumb_image)
        return true;
    auto name = element.name.view();
    return name == "progress"sv || name == "volume"sv || name == "balance"sv
        || name == "preamp"sv || name.starts_with("eqfactor"sv) || name == "surround"sv;
}

float SkinCanvas::ratio_at(SkinElement const& element, Gfx::IntPoint point) const
{
    if (element.vertical) {
        if (element.bounds.height() <= 0)
            return 0.0f;
        return clamp_ratio(1.0f - static_cast<float>(point.y() - element.bounds.y()) / element.bounds.height());
    }
    if (element.bounds.width() <= 0)
        return 0.0f;
    return clamp_ratio(static_cast<float>(point.x() - element.bounds.x()) / element.bounds.width());
}

int SkinCanvas::frame_state(SkinElement const& element) const
{
    if (m_disabled.get(element.name).value_or(false))
        return 3; // disabled
    if (m_pressed == element.name)
        return 2; // pressed
    if (m_hovered == element.name)
        return 1; // hover
    return 0;     // normal
}

void SkinCanvas::paint(GUI::Painter& painter, [[maybe_unused]] Gfx::IntRect const& clip, Gfx::Font const& default_font, Gfx::IntSize panel_size)
{
    if (!m_layout.has_value())
        return;
    auto const& layout = m_layout.value();
    if (layout.background)
        paint_background(painter, layout, panel_size);

    // PlayerSkinOrder is front-to-back, so paint in reverse: the first element
    // in the vector must end up on top of the later ones.
    for (size_t i = layout.elements.size(); i > 0; --i)
        paint_element(painter, default_font, layout.elements[i - 1]);
}

// Resizable native panels (playlist_window / lyric_window) ship a SHORT
// backdrop: 268x90 for a 268x330 playlist. `resize_rect` marks the band that
// repeats and `resize_tile` says to tile it rather than stretch it. Blitting
// the bitmap 1:1 left the rest of the window as raw widget background, which
// is the beige band seen under the playlist and the lyrics.
void SkinCanvas::paint_background(GUI::Painter& painter, SkinPanelLayout const& layout, Gfx::IntSize panel_size)
{
    auto const& background = *layout.background;
    auto panel = Gfx::IntRect { {}, panel_size };
    if (panel.is_empty())
        return;

    if (background.size() == panel_size) {
        painter.blit({}, background, background.rect());
        return;
    }

    // No repeat band declared: the skin simply expects its picture to fill the
    // window, so scale it.
    auto band = layout.resize_rect.intersected(background.rect());
    if (band.is_empty() || !layout.resize_tile) {
        if (auto scaled = background.scaled_to_size(panel_size); !scaled.is_error())
            painter.blit({}, *scaled.value(), scaled.value()->rect());
        return;
    }

    // Nine-slice: the corners/edges keep their size, the middle band repeats.
    auto tile = [&](Gfx::IntRect const& source_rect, Gfx::IntRect const& destination_rect) {
        if (source_rect.is_empty() || destination_rect.is_empty())
            return;
        for (int y = destination_rect.y(); y < destination_rect.bottom(); y += source_rect.height()) {
            for (int x = destination_rect.x(); x < destination_rect.right(); x += source_rect.width()) {
                auto width = min(source_rect.width(), destination_rect.right() - x);
                auto height = min(source_rect.height(), destination_rect.bottom() - y);
                painter.blit({ x, y }, background, { source_rect.x(), source_rect.y(), width, height });
            }
        }
    };

    // Horizontal bands: left cap / repeating middle / right cap.
    Gfx::IntRect left { 0, 0, band.x(), background.height() };
    Gfx::IntRect middle { band.x(), 0, band.width(), background.height() };
    Gfx::IntRect right { band.right(), 0, background.width() - band.right(), background.height() };

    int middle_width = max(0, panel.width() - left.width() - right.width());
    if (left.width() > 0)
        painter.blit({ 0, 0 }, background, left);
    if (middle_width > 0)
        tile(middle, { left.width(), 0, middle_width, background.height() });
    if (right.width() > 0)
        painter.blit({ panel.width() - right.width(), 0 }, background, right);

    // Vertical repeat of the whole (already horizontally completed) strip.
    int bottom_cap = background.height() - band.bottom();
    int strip_height = background.height() - bottom_cap;
    if (strip_height > 0) {
        for (int y = background.height(); y < panel.height() - bottom_cap; y += strip_height) {
            auto height = min(strip_height, panel.height() - bottom_cap - y);
            if (height <= 0)
                break;
            painter.blit({ 0, y }, background, { 0, 0, background.width(), min(height, strip_height) });
        }
    }
    if (bottom_cap > 0 && panel.height() - bottom_cap >= 0)
        painter.blit({ 0, panel.height() - bottom_cap }, background,
            { 0, background.height() - bottom_cap, background.width(), bottom_cap });
}

Optional<Gfx::IntRect> SkinCanvas::element_bounds(StringView name) const
{
    if (!m_layout.has_value())
        return {};
    for (auto const& element : m_layout->elements) {
        if (element.name.view() == name && !element.bounds.is_empty())
            return element.bounds;
    }
    return {};
}

void SkinCanvas::paint_element(GUI::Painter& painter, Gfx::Font const& default_font, SkinElement const& element)
{
    if (element.bounds.is_empty())
        return;
    if (is_slider(element)) {
        paint_slider(painter, element);
        return;
    }
    if (element.name.view() == "led"sv && element.image && element.frames == 12) {
        paint_led(painter, element);
        return;
    }
    if (element.image) {
        auto source = element.frames > 1
            ? element.frame_source_rect(frame_state(element))
            : element.image->rect();
        blit_skipping_key(painter, element.bounds.location(), *element.image, source, mask_color());
        return;
    }
    paint_text(painter, default_font, element);
}

void SkinCanvas::paint_led(GUI::Painter& painter, SkinElement const& element)
{
    auto text = m_texts.get(element.name);
    if (!text.has_value() || text->is_empty())
        return;
    auto glyph = element.frame_size();
    if (glyph.is_empty())
        return;

    // number.bmp is 12 horizontally packed glyphs: 0-9, ':', '-'.
    auto index_of = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c == ':') return 10;
        if (c == '-') return 11;
        return -1;
    };
    int count = 0;
    for (char c : text->view()) {
        if (index_of(c) >= 0)
            ++count;
    }

    int x = element.bounds.x();
    if ((element.alignment & 0x0fU) == 3) // align="right"
        x = element.bounds.right() - count * glyph.width();
    for (char c : text->view()) {
        int index = index_of(c);
        if (index < 0)
            continue; // spaces and '/' are just gaps in the LED
        blit_skipping_key(painter, Gfx::IntPoint { x, element.bounds.y() }, *element.image,
            Gfx::IntRect { index * glyph.width(), 0, glyph.width(), glyph.height() }, mask_color());
        x += glyph.width();
    }
}

// Native slider grips are a four-state strip laid out left-to-right (normal,
// hover, pressed, disabled), exactly like button images. Blitting the whole
// bitmap painted all four at once, which is why the progress bar and the
// volume slider looked like a row of little boxes instead of one grip.
Gfx::IntRect thumb_frame_rect(SkinElement const& element, int state)
{
    if (!element.thumb_image)
        return {};
    auto width = element.thumb_image->width();
    auto height = element.thumb_image->height();
    constexpr int frame_count = 4;
    // Do not require width % 4 == 0: real skins pack the four states with a
    // 1px separator, so a 37x5 grip is four 8px frames on a 9px pitch. The
    // old exact-division test rejected it and blitted all 37px, which turned
    // ten separate grips into one continuous bar across the equalizer.
    auto frame_width = width / frame_count;
    if (frame_width < 2)
        return element.thumb_image->rect();
    state = clamp(state, 0, frame_count - 1);
    return { state * frame_width, 0, frame_width, height };
}

void SkinCanvas::paint_slider(GUI::Painter& painter, SkinElement const& element)
{
    auto value = clamp_ratio(m_values.get(element.name).value_or(0.0f));

    if (element.bar_image)
        blit_skipping_key(painter, element.bounds.location(), *element.bar_image, element.bar_image->rect(), mask_color());

    if (element.fill_image) {
        auto source = element.fill_image->rect();
        if (element.vertical) {
            int height = static_cast<int>(source.height() * value);
            source = Gfx::IntRect { source.x(), source.bottom() - height, source.width(), height };
        } else {
            source.set_width(static_cast<int>(source.width() * value));
        }
        if (!source.is_empty()) {
            // A vertical fill grows from the bottom of the groove. The fill
            // bitmap is thinner than the groove (eq_fill is 9px in an 18px
            // slot, volume_fill 6px in an 18px one), so it has to be centred
            // on the cross axis - drawn at the groove's top-left corner the
            // green bar hung off the left/top edge instead of inside it.
            auto destination = element.bounds.location();
            if (element.vertical) {
                destination.set_x(element.bounds.x() + (element.bounds.width() - source.width()) / 2);
                destination.set_y(element.bounds.bottom() - source.height());
            } else {
                destination.set_y(element.bounds.y() + (element.bounds.height() - source.height()) / 2);
            }
            blit_skipping_key(painter, destination, *element.fill_image, source, mask_color());
        }
    }

    if (element.thumb_image) {
        auto thumb = thumb_frame_rect(element, frame_state(element));
        Gfx::IntPoint position;
        if (element.vertical) {
            position = Gfx::IntPoint {
                element.bounds.x() + (element.bounds.width() - thumb.width()) / 2,
                element.bounds.y() + static_cast<int>((element.bounds.height() - thumb.height()) * (1.0f - value))
            };
        } else {
            position = Gfx::IntPoint {
                element.bounds.x() + static_cast<int>((element.bounds.width() - thumb.width()) * value),
                element.bounds.y() + (element.bounds.height() - thumb.height()) / 2
            };
        }
        blit_skipping_key(painter, position, *element.thumb_image, thumb, mask_color());
    }
}

void SkinCanvas::paint_text(GUI::Painter& painter, Gfx::Font const& default_font, SkinElement const& element)
{
    auto text = m_texts.get(element.name);
    if (!text.has_value() || text->is_empty())
        return;
    // NOTE: the native `font` attribute is a Windows LOGFONT descriptor
    // ("-11,0,...,Tahoma"); until a LOGFONT->Gfx::Font mapping exists we use
    // the widget font, but honour the skin's colour and alignment.
    painter.draw_text(Gfx::FloatRect { element.bounds.x(), element.bounds.y(), element.bounds.width(), element.bounds.height() },
        text->view(), default_font, to_text_alignment(element.alignment), element.color);
}

bool SkinCanvas::mousemove(Gfx::IntPoint point)
{
    if (!m_dragging.is_empty()) {
        if (auto const* element = find(m_dragging.view())) {
            auto ratio = ratio_at(*element, point);
            m_values.set(element->name, ratio);
            invalidate();
            if (on_slider)
                on_slider(element->name.view(), ratio);
        }
        return true;
    }
    auto const* hit = hit_test(point);
    ByteString hovered = hit ? hit->name : ByteString {};
    if (hovered != m_hovered) {
        m_hovered = move(hovered);
        invalidate();
    }
    return hit != nullptr;
}

bool SkinCanvas::mousedown(Gfx::IntPoint point, bool primary_button)
{
    if (!primary_button)
        return false;
    auto const* hit = hit_test(point);
    if (!hit)
        return false;

    m_pressed = hit->name;
    if (is_slider(*hit)) {
        m_dragging = hit->name;
        auto ratio = ratio_at(*hit, point);
        m_values.set(hit->name, ratio);
        if (on_slider)
            on_slider(hit->name.view(), ratio);
    }
    invalidate();
    return true;
}

bool SkinCanvas::mouseup(Gfx::IntPoint point, bool primary_button)
{
    if (!primary_button)
        return false;
    if (!m_dragging.is_empty()) {
        m_dragging = {};
        m_pressed = {};
        invalidate();
        return true;
    }
    if (!m_pressed.is_empty()) {
        auto const* hit = hit_test(point);
        if (hit && hit->name == m_pressed && !m_disabled.get(m_pressed).value_or(false) && on_action)
            on_action(m_pressed.view());
    }
    m_pressed = {};
    invalidate();
    return true;
}

void SkinCanvas::leave()
{
    m_hovered = {};
    m_pressed = {};
    invalidate();
}
