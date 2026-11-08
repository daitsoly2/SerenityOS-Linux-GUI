/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SkinDocument.h"
#include "TTSkin.h"
#include <AK/RefPtr.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>

// Skin discovery/loading for the 千千静听 windows.
//
// The on-disk layout follows what the original player (and the M4Player clone)
// use: one directory per skin holding a manifest plus background bitmaps.
// Manifests understood, in order of preference:
//
//   Skin.xml        NATIVE 千千静听 format (version 2): describes the windows
//                   AND every control (rect / multi-frame bitmap / colours /
//                   font). Parsed by SkinDocument, exposed via native_document().
//   skin.ini        [Skin] Name=/Author=, [Colors] ..., [Images] ..., [Layout] ...
//   Skin_OOP.xml    M4Player/TT2OO: <name> + <transparent_color>; each panel's
//                   XRC (player_window.xml, PlayList.xml, Lyric.xml, Eq.xml)
//                   supplies its <rect> geometry and backdrop <image>
//   (none)          images auto-detected by filename (main.bmp, playlist.bmp, ...)
//
// A .tsk/.msk/.zip pack is extracted into the skins directory and then loaded
// through the same path, so original 千千静听 skin packs can be dropped in
// unchanged. Note that native `position` is two-point (l,t,r,b) while the
// M4Player XRC `<rect>` is x,y,width,height.
class SkinTheme {
public:
    enum class WindowKind {
        Main,
        Playlist,
        Lyrics,
        Equalizer,
    };

    struct Skin {
        ByteString name;
        ByteString directory;
        TTThemeColors colors;
        RefPtr<Gfx::Bitmap> main_background;
        RefPtr<Gfx::Bitmap> playlist_background;
        RefPtr<Gfx::Bitmap> lyrics_background;
        RefPtr<Gfx::Bitmap> equalizer_background;

        // [Layout] entries from skin.ini, in main-window-relative coordinates.
        // This is the M4Player <rect> equivalent: a skin owns the shape of the
        // player, not just its palette.
        Optional<Gfx::IntRect> geometry[4];

        // M4Player's <transparent_color>: the RGB that marks holes in the
        // bitmap. Passed straight to WindowServer as a colour key.
        Optional<Gfx::Color> transparent_color;

        // Present only for native Skin.xml skins. This is the control-level
        // layout; skin.ini / XRC skins leave it empty and keep using LibGUI
        // widgets instead of a SkinCanvas.
        Optional<SkinDocument> document;

        Optional<Gfx::IntRect> layout_for(WindowKind kind) const
        {
            return geometry[static_cast<int>(kind)];
        }

        Gfx::Bitmap const* background(WindowKind kind) const
        {
            switch (kind) {
            case WindowKind::Main:
                return main_background.ptr();
            case WindowKind::Playlist:
                return playlist_background.ptr();
            case WindowKind::Lyrics:
                return lyrics_background.ptr();
            case WindowKind::Equalizer:
                return equalizer_background.ptr();
            }
            return nullptr;
        }
    };

    static SkinTheme& the();

    static ByteString skins_root();
    static ByteString builtin_skins_root();

    void rescan();
    int count() const { return static_cast<int>(m_skins.size()); }
    ByteString name_at(int index) const;
    Vector<ByteString> names() const;

    // -1 selects the built-in classic look.
    int current_index() const { return m_current_index; }
    ByteString current_name() const;

    void select(int index);
    void select_by_name(StringView name);
    void load_preference();
    void save_preference() const;

    TTThemeColors const& colors() const;
    Gfx::Bitmap const* background(WindowKind) const;
    bool has_background(WindowKind) const;
    Optional<Gfx::IntRect> window_geometry(WindowKind) const;
    Optional<Gfx::Color> transparent_color() const;

    // Control-level layout for the current skin, or nullptr for classic skins.
    SkinDocument const* native_document() const;

    // Extracts a .tsk/.msk/.zip pack (or registers a directory), then rescans.
    ErrorOr<ByteString> install_package(StringView path);

private:
    SkinTheme() = default;

    void load_skin(Skin&, StringView directory);
    void load_ini_manifest(Skin&, StringView path);
    void load_oop_manifest(Skin&, StringView path);
    void detect_backgrounds(Skin&);
    static RefPtr<Gfx::Bitmap> load_bitmap(StringView path);

    Vector<Skin> m_skins;
    int m_current_index { -1 };
};
