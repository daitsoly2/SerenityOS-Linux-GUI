/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinTheme.h"
#include <AK/LexicalPath.h>
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

    auto layout_path = ByteString::formatted("{}/player_window.xml", skin.directory);
    auto layout_file = Core::File::open(layout_path, Core::File::OpenMode::Read);
    if (layout_file.is_error())
        return;
    auto layout_contents = layout_file.value()->read_until_eof();
    if (layout_contents.is_error())
        return;
    StringView layout_text { reinterpret_cast<char const*>(layout_contents.value().data()), layout_contents.value().size() };
    auto image = read_tag(layout_text, "image"sv);
    if (!image.is_empty())
        skin.main_background = load_bitmap(ByteString::formatted("{}/{}", skin.directory, image));
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
