/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinTheme.h"
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibConfig/Client.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Command.h>
#include <LibCore/DirIterator.h>
#include <LibCore/System.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>

namespace {

constexpr StringView config_domain = "TTPlayer"sv;

ByteString read_tag(StringView text, StringView tag)
{
    // Minimal "<tag>value</tag>" extraction; the manifests we care about are
    // flat, so a real XML parser would be overkill.
    auto open = ByteString::formatted("<{}>", tag);
    auto close = ByteString::formatted("</{}>", tag);
    auto start = text.find(open);
    if (!start.has_value())
        return {};
    auto value_start = *start + open.length();
    auto end = text.find(close, value_start);
    if (!end.has_value())
        return {};
    return text.substring_view(value_start, *end - value_start).trim_whitespace().to_byte_string();
}

bool has_image_extension(StringView path)
{
    return path.ends_with(".bmp"sv, CaseSensitivity::CaseInsensitive)
        || path.ends_with(".png"sv, CaseSensitivity::CaseInsensitive)
        || path.ends_with(".jpg"sv, CaseSensitivity::CaseInsensitive)
        || path.ends_with(".jpeg"sv, CaseSensitivity::CaseInsensitive);
}

// M4Player/千千静听 skin packs are 24-bit bitmaps with no alpha channel; one
// magic RGB marks the holes. Accept "255,0,255", "#ff00ff" and friends.
Optional<Gfx::Color> parse_mask_color(StringView value)
{
    value = value.trim_whitespace();
    if (value.is_empty())
        return {};
    auto parts = value.split_view(',');
    if (parts.size() == 3) {
        int channel[3];
        for (size_t i = 0; i < 3; ++i) {
            channel[i] = parts[i].trim_whitespace().to_number<int>().value_or(-1);
            if (channel[i] < 0 || channel[i] > 255)
                return {};
        }
        return Gfx::Color::from_rgb((channel[0] << 16) | (channel[1] << 8) | channel[2]);
    }
    // Hex form: #rrggbb / rrggbb / 0xrrggbb. parse_color() returns its
    // fallback on malformed input, so pick a value no sane key would use.
    constexpr u32 invalid = 0x010203;
    auto color = TTSkin::parse_color(value, Gfx::Color::from_rgb(invalid));
    if ((color.value() & 0x00ffffff) == invalid)
        return {};
    return Gfx::Color::from_rgb(color.value() & 0x00ffffff);
}

}

SkinTheme& SkinTheme::the()
{
    static SkinTheme s_theme;
    return s_theme;
}

ByteString SkinTheme::skins_root()
{
    return ByteString::formatted("{}/skins", Core::StandardPaths::config_directory());
}

ByteString SkinTheme::builtin_skins_root()
{
    return "/res/ttplayer-skins"_string.to_byte_string();
}

ByteString SkinTheme::name_at(int index) const
{
    if (index < 0 || index >= count())
        return "内置经典"_string.to_byte_string();
    return m_skins[static_cast<size_t>(index)].name;
}

ByteString SkinTheme::current_name() const
{
    return name_at(m_current_index);
}

Vector<ByteString> SkinTheme::names() const
{
    Vector<ByteString> result;
    for (auto const& skin : m_skins)
        result.append(skin.name);
    return result;
}

TTThemeColors const& SkinTheme::colors() const
{
    if (m_current_index < 0 || m_current_index >= count())
        return TTSkin::colors();
    return m_skins[static_cast<size_t>(m_current_index)].colors;
}

Gfx::Bitmap const* SkinTheme::background(WindowKind kind) const
{
    if (m_current_index < 0 || m_current_index >= count())
        return nullptr;
    return m_skins[static_cast<size_t>(m_current_index)].background(kind);
}

bool SkinTheme::has_background(WindowKind kind) const
{
    return background(kind) != nullptr;
}

Optional<Gfx::IntRect> SkinTheme::window_geometry(WindowKind kind) const
{
    if (m_current_index < 0 || m_current_index >= count())
        return {};
    return m_skins[static_cast<size_t>(m_current_index)].layout_for(kind);
}

Optional<Gfx::Color> SkinTheme::transparent_color() const
{
    if (m_current_index < 0 || m_current_index >= count())
        return {};
    return m_skins[static_cast<size_t>(m_current_index)].transparent_color;
}

SkinDocument const* SkinTheme::native_document() const
{
    if (m_current_index < 0 || m_current_index >= count())
        return nullptr;
    auto const& document = m_skins[static_cast<size_t>(m_current_index)].document;
    if (!document.has_value() || !document->valid)
        return nullptr;
    return &document.value();
}

void SkinTheme::select(int index)
{
    if (index >= count())
        index = -1;
    m_current_index = index;
    if (index < 0)
        TTSkin::set_colors(TTThemeColors {});
    else
        TTSkin::set_colors(m_skins[static_cast<size_t>(index)].colors);
    save_preference();
}

void SkinTheme::select_by_name(StringView name)
{
    for (int i = 0; i < count(); i++) {
        if (m_skins[static_cast<size_t>(i)].name == name) {
            select(i);
            return;
        }
    }
    select(-1);
}

void SkinTheme::load_preference()
{
    auto name = Config::read_string(config_domain, "Preferences"sv, "Skin"sv, ""sv);
    if (name.is_empty()) {
        select(-1);
        return;
    }
    select_by_name(name);
}

void SkinTheme::save_preference() const
{
    auto name = name_at(m_current_index);
    Config::write_string(config_domain, "Preferences"sv, "Skin"sv, m_current_index < 0 ? ""sv : name.view());
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void SkinTheme::rescan()
{
    auto current_name_before = m_current_index >= 0 ? name_at(m_current_index) : ByteString {};

    m_skins.clear();
    for (auto const& root : { skins_root(), builtin_skins_root() }) {
        Core::DirIterator iterator { root, Core::DirIterator::SkipParentAndBaseDir };
        while (iterator.has_next()) {
            auto entry = iterator.next_path();
            if (entry.is_empty())
                break;
            auto path = ByteString::formatted("{}/{}", root, entry);
            if (!FileSystem::is_directory(path))
                continue;
            Skin skin;
            skin.directory = path;
            skin.name = entry;
            load_skin(skin, path);
            m_skins.append(move(skin));
        }
    }

    // Keep the previous selection if it still exists.
    m_current_index = -1;
    for (int i = 0; i < count(); i++) {
        if (!current_name_before.is_empty() && m_skins[static_cast<size_t>(i)].name == current_name_before) {
            m_current_index = i;
            break;
        }
    }
    TTSkin::set_colors(m_current_index < 0 ? TTThemeColors {} : m_skins[static_cast<size_t>(m_current_index)].colors);
}

void SkinTheme::load_skin(Skin& skin, StringView directory)
{
    // Native Skin.xml wins: it is the only format that describes the controls,
    // not just the backdrop. A pack carrying both Skin.xml and skin.ini must
    // keep its control layout instead of degrading to a plain background.
    auto document = SkinDocument::load(directory);
    if (document.valid) {
        if (!document.name.is_empty())
            skin.name = document.name;
        skin.transparent_color = document.transparent_color;
        if (!skin.transparent_color.has_value())
            skin.transparent_color = Gfx::Color::from_rgb(0xff00ff);

        // Native panel `position` is already main-window-relative (l,t,r,b
        // converted to x,y,w,h by SkinDocument). When a window omits it the
        // backdrop bitmap itself defines the size.
        auto apply = [&](SkinPanelLayout const& layout, RefPtr<Gfx::Bitmap>& bitmap, int index) {
            if (layout.background)
                bitmap = layout.background;
            if (!layout.position.is_empty())
                skin.geometry[index] = layout.position;
            else if (layout.background)
                skin.geometry[index] = Gfx::IntRect { 0, 0, layout.background->width(), layout.background->height() };
        };
        apply(document.main, skin.main_background, 0);
        apply(document.playlist, skin.playlist_background, 1);
        apply(document.lyrics, skin.lyrics_background, 2);
        apply(document.equalizer, skin.equalizer_background, 3);

        skin.document = move(document);
        detect_backgrounds(skin);
        return;
    }

    auto ini_path = ByteString::formatted("{}/skin.ini", directory);
    auto alt_ini_path = ByteString::formatted("{}/Skin.ini", directory);
    auto oop_manifest = ByteString::formatted("{}/Skin_OOP.xml", directory);

    if (FileSystem::exists(ini_path))
        load_ini_manifest(skin, ini_path);
    else if (FileSystem::exists(alt_ini_path))
        load_ini_manifest(skin, alt_ini_path);
    else if (FileSystem::exists(oop_manifest))
        load_oop_manifest(skin, oop_manifest);

    detect_backgrounds(skin);
}

void SkinTheme::load_ini_manifest(Skin& skin, StringView path)
{
    auto maybe_config = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::No);
    if (maybe_config.is_error())
        return;
    auto config = maybe_config.release_value();

    auto name = config->read_entry("Skin"sv, "Name"sv, ""sv);
    if (!name.is_empty())
        skin.name = name;

    // [Skin] transparent_color=#ff00ff or 255,0,255. M4Player uses the same
    // key (default 255,0,255) to punch holes in its 24-bit skin bitmaps.
    auto key = config->read_entry("Skin"sv, "transparent_color"sv, ""sv);
    if (key.is_empty())
        key = config->read_entry("Skin"sv, "mask_color"sv, ""sv);
    skin.transparent_color = parse_mask_color(key);

    // Colours: every key is optional and falls back to the classic palette.
    auto& c = skin.colors;
    auto color = [&](StringView key, Gfx::Color fallback) {
        auto value = config->read_entry("Colors"sv, key, ""sv);
        if (value.is_empty())
            return fallback;
        return TTSkin::parse_color(value, fallback);
    };
    TTThemeColors defaults;
    c.window_background = color("window_bg"sv, defaults.window_background);
    c.panel_top = color("panel_top"sv, defaults.panel_top);
    c.panel_bottom = color("panel_bottom"sv, defaults.panel_bottom);
    c.inset_background = color("inset_bg"sv, defaults.inset_background);
    c.title_strip = color("title_strip"sv, defaults.title_strip);
    c.title_text = color("title_text"sv, defaults.title_text);
    c.bevel_light = color("bevel_light"sv, defaults.bevel_light);
    c.bevel_mid = color("bevel_mid"sv, defaults.bevel_mid);
    c.bevel_dark = color("bevel_dark"sv, defaults.bevel_dark);
    c.text = color("text"sv, defaults.text);
    c.dim_text = color("dim_text"sv, defaults.dim_text);
    c.faint_text = color("faint_text"sv, defaults.faint_text);
    c.accent = color("accent"sv, defaults.accent);
    c.button_face = color("button_face"sv, defaults.button_face);
    c.button_hover = color("button_hover"sv, defaults.button_hover);
    c.button_pressed = color("button_pressed"sv, defaults.button_pressed);
    c.progress_fill = color("progress_fill"sv, defaults.progress_fill);
    c.inset_text = color("inset_text"sv, defaults.inset_text);
    c.inset_dim_text = color("inset_dim_text"sv, defaults.inset_dim_text);
    c.current_row = color("current_row"sv, defaults.current_row);
    c.current_row_text = color("current_row_text"sv, defaults.current_row_text);

    // [Layout] main/playlist/lyrics/equalizer = x,y,width,height, relative to
    // the main window origin. Panel positions in M4Player skins are defined
    // the same way (OOPWindow::HackXrc reads <rect> and adds the main origin).
    auto geometry = [&](StringView key) -> Optional<Gfx::IntRect> {
        auto value = config->read_entry("Layout"sv, key, ""sv);
        auto parts = value.split_view(',');
        if (parts.size() != 4)
            return {};
        int v[4];
        for (size_t i = 0; i < 4; ++i)
            v[i] = parts[i].trim_whitespace().to_number<int>().value_or(0);
        if (v[2] <= 0 || v[3] <= 0)
            return {};
        return Gfx::IntRect { v[0], v[1], v[2], v[3] };
    };
    skin.geometry[0] = geometry("main"sv);
    skin.geometry[1] = geometry("playlist"sv);
    skin.geometry[2] = geometry("lyrics"sv);
    skin.geometry[3] = geometry("equalizer"sv);

    // Image names may be given explicitly; detect_backgrounds() fills the rest.
    skin.main_background = load_bitmap(ByteString::formatted("{}/{}", skin.directory, config->read_entry("Images"sv, "main"sv, ""sv)));
    skin.playlist_background = load_bitmap(ByteString::formatted("{}/{}", skin.directory, config->read_entry("Images"sv, "playlist"sv, ""sv)));
    skin.lyrics_background = load_bitmap(ByteString::formatted("{}/{}", skin.directory, config->read_entry("Images"sv, "lyrics"sv, ""sv)));
    skin.equalizer_background = load_bitmap(ByteString::formatted("{}/{}", skin.directory, config->read_entry("Images"sv, "equalizer"sv, ""sv)));
}

void SkinTheme::load_oop_manifest(Skin& skin, StringView path)
{
    // M4Player/OOPlayer skin descriptor: <name>...</name>, and the main window
    // bitmap lives in player_window.xml as the first <image> tag.
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return;
    StringView text { reinterpret_cast<char const*>(contents.value().data()), contents.value().size() };

    auto name = read_tag(text, "name"sv);
    if (!name.is_empty())
        skin.name = name;

    // M4Player declares the mask colour in Skin_OOP.xml; 255,0,255 is its
    // documented default even when the tag is missing.
    skin.transparent_color = parse_mask_color(read_tag(text, "transparent_color"sv));
    if (!skin.transparent_color.has_value())
        skin.transparent_color = Gfx::Color::from_rgb(0xff00ff);

    // TT2OO converts each <panel> of an original 千千静听 skin.xml into one
    // XRC file named after that panel. The root <object class="VdkWindow">
    // carries the window <rect> (x,y,width,height - see TT2OO::Pos2Rect) and
    // the backdrop <image>; child <object>s describe buttons/sliders and come
    // later in the document, so the FIRST tag of each kind is the window one.
    // Read all four panels, not just the main one: without their rects
    // apply_skin_geometry() has nothing to work with and the sticky panels
    // keep their old size/position, and without their images the player falls
    // back to filename guessing.
    struct Panel {
        Optional<Gfx::IntRect> rect;
        RefPtr<Gfx::Bitmap> bitmap;
    };
    auto load_panel = [&](Vector<StringView> names) -> Panel {
        for (auto const& name : names) {
            auto xrc_path = ByteString::formatted("{}/{}", skin.directory, name);
            auto xrc = Core::File::open(xrc_path, Core::File::OpenMode::Read);
            if (xrc.is_error())
                continue;
            auto bytes = xrc.value()->read_until_eof();
            if (bytes.is_error())
                continue;
            StringView xrc_text { reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size() };

            Panel panel;
            auto rect = read_tag(xrc_text, "rect"sv);
            auto parts = StringView { rect }.split_view(',');
            if (parts.size() == 4) {
                int v[4];
                for (size_t i = 0; i < 4; ++i)
                    v[i] = parts[i].trim_whitespace().to_number<int>().value_or(0);
                if (v[2] > 0 && v[3] > 0)
                    panel.rect = Gfx::IntRect { v[0], v[1], v[2], v[3] };
            }

            auto image = read_tag(xrc_text, "image"sv);
            if (image.is_empty())
                image = read_tag(xrc_text, "bitmap"sv);
            if (!image.is_empty()) {
                // Windows-authored skins use backslashes and subdirectories.
                StringBuilder normalized;
                for (char c : image)
                    normalized.append(c == '\\' ? '/' : c);
                auto image_path = normalized.to_byte_string();
                panel.bitmap = load_bitmap(ByteString::formatted("{}/{}", skin.directory, image_path));
                if (!panel.bitmap)
                    panel.bitmap = load_bitmap(ByteString::formatted("{}/{}", skin.directory, LexicalPath::basename(image_path)));
            }
            return panel; // first XRC that exists wins
        }
        return {};
    };

    auto main = load_panel({ "player_window.xml"sv });
    auto playlist = load_panel({ "PlayList.xml"sv, "Playlist.xml"sv, "playlist.xml"sv });
    auto lyrics = load_panel({ "Lyric.xml"sv, "Lyrics.xml"sv, "lyrics.xml"sv });
    auto equalizer = load_panel({ "Eq.xml"sv, "Equalizer.xml"sv, "equalizer.xml"sv });

    if (main.bitmap)
        skin.main_background = main.bitmap;
    if (playlist.bitmap)
        skin.playlist_background = playlist.bitmap;
    if (lyrics.bitmap)
        skin.lyrics_background = lyrics.bitmap;
    if (equalizer.bitmap)
        skin.equalizer_background = equalizer.bitmap;

    if (main.rect.has_value())
        skin.geometry[0] = main.rect;
    if (playlist.rect.has_value())
        skin.geometry[1] = playlist.rect;
    if (lyrics.rect.has_value())
        skin.geometry[2] = lyrics.rect;
    if (equalizer.rect.has_value())
        skin.geometry[3] = equalizer.rect;
}

void SkinTheme::detect_backgrounds(Skin& skin)
{
    struct Candidate {
        RefPtr<Gfx::Bitmap>& slot;
        Vector<StringView> names;
    };
    Candidate candidates[] = {
        { skin.main_background, { "main"sv, "player"sv, "player_window"sv, "bg_main"sv } },
        { skin.playlist_background, { "playlist"sv, "list"sv, "bg_playlist"sv } },
        { skin.lyrics_background, { "lyric"sv, "lyrics"sv, "bg_lyric"sv } },
        { skin.equalizer_background, { "eq"sv, "equalizer"sv, "bg_eq"sv } },
    };

    for (auto& candidate : candidates) {
        if (candidate.slot)
            continue;
        bool found = false;
        for (auto const& name : candidate.names) {
            for (auto const& extension : { ".bmp"sv, ".png"sv, ".jpg"sv }) {
                auto path = ByteString::formatted("{}/{}{}", skin.directory, name, extension);
                if (auto bitmap = load_bitmap(path)) {
                    candidate.slot = move(bitmap);
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }
}

RefPtr<Gfx::Bitmap> SkinTheme::load_bitmap(StringView path)
{
    if (path.is_empty() || !has_image_extension(path))
        return nullptr;
    auto maybe_bitmap = Gfx::Bitmap::load_from_file(path);
    if (maybe_bitmap.is_error())
        return nullptr;
    return maybe_bitmap.release_value();
}

// ---------------------------------------------------------------------------
// Installing a pack
// ---------------------------------------------------------------------------

ErrorOr<ByteString> SkinTheme::install_package(StringView path)
{
    auto root = skins_root();
    // ~/.config/TTPlayer/skins — ignore EEXIST.
    (void)Core::System::mkdir(ByteString::formatted("{}/TTPlayer", Core::StandardPaths::config_directory()), 0755);
    (void)Core::System::mkdir(root, 0755);

    auto lexical_path = LexicalPath { path };
    auto base_name = lexical_path.title();

    if (FileSystem::is_directory(path)) {
        // Already a skin directory: nothing to extract.
        rescan();
        return base_name;
    }

    auto target = ByteString::formatted("{}/{}", root, base_name);
    if (FileSystem::exists(target))
        TRY(FileSystem::remove(target, FileSystem::RecursionMode::Allowed));
    TRY(Core::System::mkdir(target, 0755));

    // .tsk/.msk are plain zip archives; let the system unzip handle the
    // compression variants instead of reimplementing the format.
    auto result = TRY(Core::command(ByteString { "unzip" }, { ByteString { "-o" }, ByteString { "-q" }, path.to_byte_string(), ByteString { "-d" }, target }, {}));
    if (result.exit_code != 0) {
        // Fall back to 7z, which also copes with the odd RAR/7z pack.
        auto fallback = TRY(Core::command(ByteString { "7z" }, { ByteString { "x" }, ByteString { "-y" }, ByteString::formatted("-o{}", target), path.to_byte_string() }, {}));
        if (fallback.exit_code != 0)
            return Error::from_string_literal("无法解压皮肤包");
    }

    rescan();
    return base_name;
}
