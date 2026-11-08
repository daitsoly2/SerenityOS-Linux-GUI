/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Geometry for laying a picture out on a printed sheet.
//
// This lives apart from PortPrint.h (which pulls in LibGUI widgets) so that
// *both* the print dialog preview (client) and serenity-print-service (server)
// can use the exact same placement math. The on-screen preview and the real
// sheet must always agree, and one definition is the only way to guarantee it.

#include <AK/ByteString.h>
#include <AK/Math.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Rect.h>

namespace PortPrint {

// Compose the page at a fixed canvas DPI; the driver then maps the canvas 1:1
// onto the sheet, so this only needs to be high enough to look crisp.
inline int const print_canvas_dpi = 150;

struct PagePlacement {
    bool rotated { false };
    float scale { 1.0f };
    Gfx::IntRect destination;
};

inline Gfx::IntSize page_canvas_size(int width_mm, int height_mm)
{
    auto to_px = [](int mm) { return max(1, static_cast<int>(mm / 25.4f * print_canvas_dpi)); };
    return { to_px(width_mm), to_px(height_mm) };
}

// Scaling modes every desktop print dialog offers:
//   fit      - whole picture visible, letterboxed
//   shrink   - like fit, but never enlarges (small pictures stay small)
//   fill     - picture covers the sheet, edges cropped
//   original - 100% of the image, centred, cropped by the sheet
//   custom   - a user chosen percentage
inline PagePlacement compute_placement(int image_width, int image_height, Gfx::IntSize canvas, StringView scaling, int percent)
{
    // 4% unprintable margin, like a real printer's hardware margins.
    auto printable = Gfx::IntRect { 0, 0, canvas.width(), canvas.height() }
                         .shrunken(max(2, canvas.width() / 25), max(2, canvas.height() / 25));

    float fit_upright = min(static_cast<float>(printable.width()) / image_width,
        static_cast<float>(printable.height()) / image_height);
    float fit_rotated = min(static_cast<float>(printable.width()) / image_height,
        static_cast<float>(printable.height()) / image_width);
    float cover_upright = max(static_cast<float>(printable.width()) / image_width,
        static_cast<float>(printable.height()) / image_height);
    float cover_rotated = max(static_cast<float>(printable.width()) / image_height,
        static_cast<float>(printable.height()) / image_width);

    PagePlacement placement;
    if (scaling == "fill"sv) {
        // Cover: rotating only helps if it makes the picture cover *more*.
        placement.rotated = cover_rotated < cover_upright;
        placement.scale = placement.rotated ? cover_rotated : cover_upright;
    } else if (scaling == "original"sv) {
        placement.scale = 1.0f; // 1 image pixel = 1 canvas pixel at print_canvas_dpi
    } else if (scaling == "custom"sv) {
        placement.scale = max(0.05f, min(4.0f, percent / 100.0f));
    } else {
        // fit / shrink: turn the picture if that fills more of the sheet.
        placement.rotated = fit_rotated > fit_upright;
        placement.scale = placement.rotated ? fit_rotated : fit_upright;
        if (scaling == "shrink"sv)
            placement.scale = min(placement.scale, 1.0f); // never enlarge
    }

    int width = static_cast<int>((placement.rotated ? image_height : image_width) * placement.scale);
    int height = static_cast<int>((placement.rotated ? image_width : image_height) * placement.scale);
    placement.destination = { 0, 0, max(1, width), max(1, height) };
    placement.destination.center_within(printable);
    return placement;
}

// 90 degree clockwise copy, built by the caller once per (image, orientation).
inline RefPtr<Gfx::Bitmap const> rotate_quarter_turn(Gfx::Bitmap const& source)
{
    auto rotated_or_error = Gfx::Bitmap::create(source.format(), { source.height(), source.width() });
    if (rotated_or_error.is_error())
        return nullptr;
    auto rotated = rotated_or_error.release_value();
    for (int y = 0; y < rotated->height(); ++y) {
        for (int x = 0; x < rotated->width(); ++x)
            rotated->set_pixel(x, y, source.get_pixel(y, source.height() - 1 - x));
    }
    return rotated;
}

}
