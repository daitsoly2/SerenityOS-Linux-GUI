/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>

// Parser for the *native* 千千静听 Skin.xml (version 2) format.
//
// Field names and semantics are taken from TTPlayerRebuild's src/skin/skin.cpp,
// which was recovered from the original 5.7.9 binary (CSkinParser_Parse*).
// The important difference from our skin.ini / M4Player XRC path: the skin
// owns every control - each element has its own rectangle, multi-frame bitmap,
// colours, font and alignment. A skin that only supplies a background picture
// can never look like the original player.
//
//   <skin version="2" name="..." transparent_color="#ff00ff">
//     <player_window image="main.bmp">
//       <play   position="l,t,r,b" image="play.bmp" .../>
//       <led    position="l,t,r,b" color="#8bbac6" align="left+vcenter"/>
//       <volume position="l,t,r,b" bar_image="..." fill_image="..."
//               thumb_image="..." vertical="true"/>
//       ...
//     </player_window>
//     <playlist_window image="pl.bmp" position="l,t,r,b">...</playlist_window>
//     <lyric_window     image="lr.bmp" position="l,t,r,b">...</lyric_window>
//     <equalizer_window image="eq.bmp" position="l,t,r,b">...</equalizer_window>
//   </skin>
//
// NOTE: native `position` is TWO POINTS (left,top,right,bottom), while the
// TT2OO/M4Player XRC <rect> is x,y,width,height. Do not mix them up.

struct SkinElement {
    ByteString name;
    Gfx::IntRect bounds {};

    // A button's `image` is four frames laid out left-to-right:
    // normal / hover / pressed / disabled. Sliders additionally use
    // bar/fill/fill2/thumb bitmaps.
    RefPtr<Gfx::Bitmap> image;
    RefPtr<Gfx::Bitmap> bar_image;
    RefPtr<Gfx::Bitmap> fill_image;
    RefPtr<Gfx::Bitmap> fill_image2;
    RefPtr<Gfx::Bitmap> thumb_image;
    RefPtr<Gfx::Bitmap> flash_image;

    Gfx::Color color { 255, 255, 255 };  // `color`, used by text/LED
    Gfx::Color background { 0, 0, 0 };   // `bkgnd`
    unsigned alignment {};               // left/center/right | top/vcenter/bottom
    ByteString font;
    int font_size { 12 };

    bool four_state {};                  // button with 4 frames
    bool vertical {};                    // vertical slider
    int frames { 1 };

    // `flash_*` animation (6.x skins).
    int flash_mode {};
    int frame_count { 10 };
    int frame_interval { 100 };

    Gfx::IntSize frame_size() const;
    Gfx::IntRect frame_source_rect(int state) const;
};

struct SkinPanelLayout {
    Gfx::IntRect position {};
    Gfx::IntRect resize_rect {};
    // `eq_interval`: horizontal gap between two neighbouring EQ bands. The
    // native document declares ONE <eqfactor> slider and repeats it ten times
    // (TTPlayerRebuild's CSkinParser does the same); the parser expands it, so
    // the canvas only ever sees the ten real sliders.
    int eq_interval { 0 };
    // The skin's `transparent_color`. Control bitmaps paint their background
    // in this colour (round buttons are drawn on a magenta square), so the
    // canvas must skip those pixels instead of letting them reach the backing
    // store, where the window-level colour key would punch them out into the
    // desktop and leave a dark square behind every round button.
    Optional<Gfx::Color> transparent_color;
    // `resize_tile="1"`: repeat `resize_rect` instead of stretching it when the
    // window is bigger than the backdrop bitmap.
    bool resize_tile { false };
    RefPtr<Gfx::Bitmap> background;
    Vector<SkinElement> elements;        // front-to-back z-order
    bool valid {};
};

struct SkinDocument {
    ByteString name;
    ByteString author;
    Optional<Gfx::Color> transparent_color;
    SkinPanelLayout main;
    SkinPanelLayout playlist;
    SkinPanelLayout lyrics;
    SkinPanelLayout equalizer;
    bool valid {};

    // Never fails hard: an unreadable/unsupported document returns valid=false
    // so SkinTheme can fall back to skin.ini / Skin_OOP.xml.
    static SkinDocument load(StringView directory);
};
