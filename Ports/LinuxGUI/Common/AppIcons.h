/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// One icon resolver for the whole Linux GUI port.
//
// Before this existed there were four unrelated icon lookups: AppCatalog only
// did Bitmap::load_from_file(path), StartMenuShortcuts only loaded a path,
// SystemMenu hardcoded three hicolor guesses, and LibDesktop::AppFile guessed
// /res/icons names. None of them understood a freedesktop icon *name*
// ("firefox-esr"), an .ico, or an icon embedded in a Windows executable, which
// is why Linux apps and .exe files showed a grey square.
//
// This header is header-only on purpose (like WinApps.h / ExplorerMenus.h):
// it is included by LibGUI (so FileManager, properties and previews get the
// behaviour for free) as well as by the shell's own widgets. No new library
// target / link dependency is introduced.
//
// Resolution priority for an application shortcut:
//   1. Icon= / IconPath= from the shortcut (absolute path or theme name)
//   2. the executable itself (PE resource icon for .exe/.msi)
//   3. generic file-type icon (caller's fallback)
//
// The PE reader has two layers, mirroring omikuji-core's media/mod.rs:
//   * walk the real .rsrc tree (RT_GROUP_ICON -> RT_ICON) and rebuild an ICO;
//   * if that fails (packed / malformed PE), scan the file for PNG signatures
//     and embedded ICONDIR headers, keeping the largest plausible image.
// Vista+ icons are stored as raw PNG resources, older ones as ICONDIR.

#include <AK/Base64.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Command.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/MappedFile.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace PortIcons {

// ---------------------------------------------------------------------------
// Small helpers

inline bool read_u16(ReadonlyBytes bytes, size_t offset, u16& out)
{
    if (offset + 2 > bytes.size())
        return false;
    out = static_cast<u16>(bytes[offset]) | (static_cast<u16>(bytes[offset + 1]) << 8);
    return true;
}

inline bool read_u32(ReadonlyBytes bytes, size_t offset, u32& out)
{
    if (offset + 4 > bytes.size())
        return false;
    out = static_cast<u32>(bytes[offset])
        | (static_cast<u32>(bytes[offset + 1]) << 8)
        | (static_cast<u32>(bytes[offset + 2]) << 16)
        | (static_cast<u32>(bytes[offset + 3]) << 24);
    return true;
}

inline void push_u16(Vector<u8>& out, u16 value)
{
    out.append(static_cast<u8>(value & 0xff));
    out.append(static_cast<u8>(value >> 8));
}

inline void push_u32(Vector<u8>& out, u32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<u8>((value >> (8 * i)) & 0xff));
}

inline void push_bytes(Vector<u8>& out, ReadonlyBytes bytes)
{
    for (auto byte : bytes)
        out.append(byte);
}

// Always return a bitmap we own: several call sites keep the bitmap after the
// decoder / Icon object that produced it has gone away.
inline RefPtr<Gfx::Bitmap const> fit(Gfx::Bitmap const& source, int size)
{
    if (size < 8 || source.width() < 1 || source.height() < 1)
        return {};
    if (source.width() == size && source.height() == size) {
        if (auto clone = source.clone(); !clone.is_error())
            return clone.release_value();
        return {};
    }
    auto sx = static_cast<float>(size) / static_cast<float>(source.width());
    auto sy = static_cast<float>(size) / static_cast<float>(source.height());
    if (auto scaled = source.scaled(sx, sy); !scaled.is_error())
        return scaled.release_value();
    return {};
}

inline RefPtr<Gfx::Bitmap const> decode_bytes(ReadonlyBytes bytes, int size)
{
    auto decoder_or_error = Gfx::ImageDecoder::try_create_for_raw_bytes(bytes);
    if (decoder_or_error.is_error() || !decoder_or_error.value())
        return {};
    auto frame_or_error = decoder_or_error.value()->frame(0, Gfx::IntSize { size, size });
    if (frame_or_error.is_error() || !frame_or_error.value().image)
        return {};
    return fit(*frame_or_error.value().image, size);
}

inline RefPtr<Gfx::Bitmap const> decode_file(ByteString const& path, int size)
{
    auto bitmap = Gfx::Bitmap::load_from_file(path, 1, Gfx::IntSize { size, size });
    if (bitmap.is_error())
        return {};
    return fit(*bitmap.value(), size);
}

// ---------------------------------------------------------------------------
// SVG rasterisation
//
// The VM has no rsvg-convert / ImageMagick, but python3-gi + GdkPixbuf loads
// SVG fine. Try the fast native tools first, then the python helper. The result
// is cached on disk so the (slow) python process runs at most once per icon.

constexpr char const* SVG_RASTERIZER = R"PY(
import sys
src, out, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
ok = False
try:
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf
    GdkPixbuf.Pixbuf.new_from_file_at_size(src, size, size).savev(out, "png", [], [])
    ok = True
except Exception:
    pass
if not ok:
    try:
        import cairosvg
        cairosvg.svg2png(url=src, write_to=out, output_width=size, output_height=size)
        ok = True
    except Exception:
        pass
sys.exit(0 if ok else 1)
)PY";

// Core::command() VERIFYs (and aborts the process) when the program cannot be
// spawned, so a missing rsvg-convert must never be handed to it. Resolve the
// tool on $PATH first and only then run it.
inline ByteString tool_on_path(StringView name)
{
    char const* path_env = getenv("PATH");
    if (!path_env || !*path_env)
        path_env = "/usr/local/bin:/usr/bin:/bin";
    for (auto directory : StringView { path_env, strlen(path_env) }.split_view(':')) {
        if (directory.is_empty())
            continue;
        auto candidate = ByteString::formatted("{}/{}", directory, name);
        if (access(candidate.characters(), X_OK) == 0)
            return candidate;
    }
    return {};
}

inline unsigned hash_string(StringView text)
{
    unsigned hash = 2166136261u;
    for (auto ch : text) {
        hash ^= static_cast<unsigned>(static_cast<u8>(ch));
        hash *= 16777619u;
    }
    return hash;
}

inline ByteString cache_directory()
{
    if (auto* value = getenv("XDG_CACHE_HOME"); value && *value)
        return ByteString::formatted("{}/serenity/icons", value);
    auto home = Core::StandardPaths::home_directory();
    if (home.is_empty())
        home = "/tmp";
    return ByteString::formatted("{}/.cache/serenity/icons", home);
}

inline ByteString rasterize_svg(ByteString const& path, int size)
{
    auto directory = cache_directory();
    if (Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes).is_error())
        return {};
    auto key = ByteString::formatted("{}@{}", path, size);
    auto target = ByteString::formatted("{}/{:x}-{}.png", directory, hash_string(key), size);
    if (FileSystem::is_regular_file(target))
        return target;

    auto size_string = ByteString::number(size);
    auto try_command = [&](StringView program_name, Vector<ByteString> arguments) {
        auto program = tool_on_path(program_name);
        if (program.is_empty())
            return false;
        auto result = Core::command(program, arguments, Optional<LexicalPath> {});
        return !result.is_error() && result.value().exit_code == 0 && FileSystem::is_regular_file(target);
    };

    if (try_command("rsvg-convert"sv, { "-w", size_string, "-o", target, path }))
        return target;
    if (try_command("convert"sv, { "-background", "none", "-resize",
             ByteString::formatted("{}x{}", size, size), path, target }))
        return target;
    if (try_command("magick"sv, { "-background", "none", "-resize",
             ByteString::formatted("{}x{}", size, size), path, target }))
        return target;
    if (try_command("python3"sv, { "-c", ByteString { SVG_RASTERIZER }, path, target, size_string }))
        return target;
    return {};
}

// ---------------------------------------------------------------------------
// Freedesktop icon theme lookup
//
// Mirrors omikuji-core's resolve_icon_value() but scores by closeness to the
// requested size rather than always picking the biggest raster, because these
// icons are drawn at 16/32/48/64 and upscaling a 16px PNG to 64 looks awful.

inline Vector<ByteString> icon_search_bases()
{
    Vector<ByteString> bases;
    auto add = [&](ByteString path) {
        if (path.is_empty())
            return;
        for (auto const& existing : bases) {
            if (existing == path)
                return;
        }
        bases.append(move(path));
    };
    auto home = Core::StandardPaths::home_directory();
    if (!home.is_empty()) {
        add(ByteString::formatted("{}/.icons", home));
        add(ByteString::formatted("{}/.local/share/icons", home));
    }
    if (auto* data_home = getenv("XDG_DATA_HOME"); data_home && *data_home)
        add(ByteString::formatted("{}/icons", data_home));
    add(ByteString { "/usr/local/share/icons" });
    add(ByteString { "/usr/share/icons" });
    add(ByteString { "/usr/share/pixmaps" });
    if (auto* data_dirs = getenv("XDG_DATA_DIRS"); data_dirs && *data_dirs) {
        for (auto part : StringView { data_dirs, strlen(data_dirs) }.split_view(':')) {
            if (!part.is_empty())
                add(ByteString::formatted("{}/icons", part));
        }
    }
    return bases;
}

struct ThemeCandidate {
    int score { -1 };
    ByteString path;
};

inline void consider(ThemeCandidate& best, int score, ByteString path)
{
    if (score > best.score) {
        best.score = score;
        best.path = move(path);
    }
}

// "48x48", "48x48@2x", "scalable", "256x256" -> pixel size (0 for unknown).
inline int directory_pixel_size(StringView name)
{
    if (name == "scalable"sv)
        return 512;
    auto x = name.find('x');
    if (!x.has_value() || x.value() == 0)
        return 0;
    return name.substring_view(0, x.value()).to_number<int>().value_or(0);
}

inline ByteString current_icon_theme()
{
    for (auto* variable : { "XDG_ICON_THEME", "GTK_THEME", "SERENITY_ICON_THEME" }) {
        if (auto* value = getenv(variable); value && *value)
            return ByteString { value };
    }
    return {};
}

inline ByteString resolve_icon_file(StringView spec, int size)
{
    if (spec.is_empty())
        return {};

    // Absolute paths are used as-is; an extension-less Icon= may point at a
    // name in /usr/share/pixmaps with any of the common extensions.
    if (spec.starts_with('/')) {
        ByteString path { spec };
        if (FileSystem::is_regular_file(path))
            return path;
        for (auto extension : { ".png"sv, ".svg"sv, ".xpm"sv, ".ico"sv }) {
            auto candidate = ByteString::formatted("{}{}", spec, extension);
            if (FileSystem::is_regular_file(candidate))
                return candidate;
        }
        return {};
    }

    // A bare name with an explicit extension is a pixmaps-style file name.
    ByteString name { spec };
    for (auto extension : { ".png"sv, ".svg"sv, ".xpm"sv, ".ico"sv }) {
        if (name.ends_with(extension))
            name = name.substring_view(0, name.length() - extension.length()).to_byte_string();
    }
    if (name.is_empty())
        return {};

    auto preferred_theme = current_icon_theme();
    ThemeCandidate best;
    constexpr int PNG_BASE = 1'000'000;
    constexpr int SVG_BASE = 500'000;
    constexpr int HICOLOR_BONUS = 50'000;
    constexpr int THEME_BONUS = 40'000;

    auto probe = [&](ByteString path, int pixels, bool is_png, int bonus) {
        if (!FileSystem::is_regular_file(path))
            return;
        int distance = pixels >= size ? pixels - size : size - pixels + 500;
        int score = (is_png ? PNG_BASE : SVG_BASE) + bonus + (10'000 - min(distance, 9'999));
        consider(best, score, move(path));
    };

    for (auto const& base : icon_search_bases()) {
        for (auto const& extension : { "png"sv, "svg"sv, "xpm"sv }) {
            auto path = ByteString::formatted("{}/{}.{}", base, name, extension);
            probe(path, 256, extension == "png"sv, 5'000);
        }

        Core::DirIterator themes { base, Core::DirIterator::SkipDots };
        if (themes.has_error())
            continue;
        while (themes.has_next()) {
            auto theme_name = themes.next_path();
            auto theme_dir = ByteString::formatted("{}/{}", base, theme_name);
            if (!FileSystem::is_directory(theme_dir))
                continue;

            int bonus = 0;
            if (theme_name == "hicolor"sv)
                bonus += HICOLOR_BONUS;
            if (!preferred_theme.is_empty() && theme_name == preferred_theme)
                bonus += THEME_BONUS;

            // Honour index.theme inheritance without walking the whole graph.
            auto index = Core::ConfigFile::open(ByteString::formatted("{}/index.theme", theme_dir));
            if (!index.is_error()) {
                auto inherits = index.value()->read_entry("Icon Theme", "Inherits");
                if (!preferred_theme.is_empty() && inherits.contains(preferred_theme))
                    bonus += THEME_BONUS / 2;
            }

            Core::DirIterator size_dirs { theme_dir, Core::DirIterator::SkipDots };
            if (!size_dirs.has_error()) {
                while (size_dirs.has_next()) {
                    auto sub = size_dirs.next_path();
                    int pixels = directory_pixel_size(sub);
                    if (pixels == 0)
                        continue;
                    for (auto const& category : { "apps"sv, "actions"sv, "devices"sv, "places"sv, "categories"sv, "mimetypes"sv }) {
                        for (auto const& extension : { "png"sv, "svg"sv, "xpm"sv }) {
                            auto path = ByteString::formatted("{}/{}/{}/{}.{}", theme_dir, sub, category, name, extension);
                            probe(path, pixels, extension == "png"sv, bonus);
                        }
                    }
                }
            }
            for (auto const& extension : { "png"sv, "svg"sv, "xpm"sv }) {
                auto path = ByteString::formatted("{}/apps/{}.{}", theme_dir, name, extension);
                probe(path, 64, extension == "png"sv, bonus);
            }
        }
    }
    return best.path;
}

// ---------------------------------------------------------------------------
// Windows PE resources
//
// Rebuild an .ico from RT_GROUP_ICON + RT_ICON and let LibGfx's ICO decoder
// (which handles both PNG-encoded and DIB icon frames) pick the largest frame.

struct PESection {
    u32 virtual_address { 0 };
    u32 virtual_size { 0 };
    u32 raw_size { 0 };
    u32 raw_offset { 0 };
};

struct ResourceEntry {
    u32 id { 0 };
    u32 offset { 0 };
    bool is_directory { false };
    bool is_named { false };
};

inline size_t rva_to_offset(u32 rva, Vector<PESection> const& sections, size_t file_size)
{
    for (auto const& section : sections) {
        u32 span = max(section.virtual_size, section.raw_size);
        if (rva >= section.virtual_address && rva - section.virtual_address < span) {
            size_t offset = static_cast<size_t>(section.raw_offset) + (rva - section.virtual_address);
            if (offset < file_size)
                return offset;
            return file_size;
        }
    }
    return file_size;
}

inline Vector<ResourceEntry> resource_entries(ReadonlyBytes file, size_t directory_offset)
{
    Vector<ResourceEntry> entries;
    u16 named_count = 0;
    u16 id_count = 0;
    if (!read_u16(file, directory_offset + 12, named_count))
        return entries;
    if (!read_u16(file, directory_offset + 14, id_count))
        return entries;
    size_t count = static_cast<size_t>(named_count) + id_count;
    if (count > 4096)
        return entries;
    for (size_t i = 0; i < count; ++i) {
        size_t entry_offset = directory_offset + 16 + i * 8;
        u32 name = 0;
        u32 offset = 0;
        if (!read_u32(file, entry_offset, name) || !read_u32(file, entry_offset + 4, offset))
            break;
        entries.append({ name & 0x7fffffff, offset & 0x7fffffff,
            (offset & 0x80000000) != 0, (name & 0x80000000) != 0 });
    }
    return entries;
}

// Walk type -> name -> language and return the raw resource bytes.
// name_id empty means "first entry at that level" (RT_GROUP_ICON names vary).
inline ReadonlyBytes resource_data(ReadonlyBytes file, size_t rsrc_offset,
    Vector<PESection> const& sections, u32 type_id, Optional<u32> name_id)
{
    auto level0 = resource_entries(file, rsrc_offset);
    ResourceEntry const* type_entry = nullptr;
    for (auto const& entry : level0) {
        if (!entry.is_named && entry.id == type_id && entry.is_directory) {
            type_entry = &entry;
            break;
        }
    }
    if (!type_entry)
        return {};

    auto level1 = resource_entries(file, rsrc_offset + type_entry->offset);
    ResourceEntry const* name_entry = nullptr;
    for (auto const& entry : level1) {
        if (!entry.is_directory)
            continue;
        if (!name_id.has_value() || (!entry.is_named && entry.id == name_id.value())) {
            name_entry = &entry;
            break;
        }
    }
    if (!name_entry)
        return {};

    auto level2 = resource_entries(file, rsrc_offset + name_entry->offset);
    ResourceEntry const* data_entry = nullptr;
    for (auto const& entry : level2) {
        if (!entry.is_directory) {
            data_entry = &entry;
            break;
        }
    }
    if (!data_entry)
        return {};

    size_t entry_offset = rsrc_offset + data_entry->offset;
    u32 data_rva = 0;
    u32 data_size = 0;
    if (!read_u32(file, entry_offset, data_rva) || !read_u32(file, entry_offset + 4, data_size))
        return {};
    if (data_size == 0 || data_size > 16 * 1024 * 1024)
        return {};
    size_t offset = rva_to_offset(data_rva, sections, file.size());
    if (offset + data_size > file.size())
        return {};
    return file.slice(offset, data_size);
}

inline ReadonlyBytes icon_from_resource_directory(ReadonlyBytes file, Vector<u8>& ico_storage)
{
    u32 pe_offset = 0;
    u32 signature = 0;
    if (file.size() < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return {};
    if (!read_u32(file, 0x3c, pe_offset) || !read_u32(file, pe_offset, signature))
        return {};
    if (signature != 0x00004550u) // "PE\0\0"
        return {};

    u16 section_count = 0;
    u16 optional_header_size = 0;
    u16 magic = 0;
    if (!read_u16(file, pe_offset + 6, section_count) || !read_u16(file, pe_offset + 20, optional_header_size)
        || !read_u16(file, pe_offset + 24, magic))
        return {};
    size_t optional_offset = static_cast<size_t>(pe_offset) + 24;
    size_t data_directories_offset = optional_offset + (magic == 0x20b ? 112 : 96);
    if (data_directories_offset + 3 * 8 > file.size())
        return {};

    u32 resource_rva = 0;
    u32 resource_size = 0;
    if (!read_u32(file, data_directories_offset + 2 * 8, resource_rva)
        || !read_u32(file, data_directories_offset + 2 * 8 + 4, resource_size))
        return {};
    if (resource_rva == 0 || resource_size == 0)
        return {};

    Vector<PESection> sections;
    size_t section_table = optional_offset + optional_header_size;
    for (u16 i = 0; i < section_count && i < 256; ++i) {
        size_t offset = section_table + static_cast<size_t>(i) * 40;
        if (offset + 40 > file.size())
            break;
        PESection section;
        u32 ignored = 0;
        (void)read_u32(file, offset + 8, section.virtual_size);
        (void)read_u32(file, offset + 12, section.virtual_address);
        (void)read_u32(file, offset + 16, section.raw_size);
        (void)read_u32(file, offset + 20, section.raw_offset);
        (void)ignored;
        sections.append(section);
    }

    size_t rsrc_offset = rva_to_offset(resource_rva, sections, file.size());
    if (rsrc_offset >= file.size())
        return {};

    auto group = resource_data(file, rsrc_offset, sections, 14, {}); // RT_GROUP_ICON
    if (group.size() < 6)
        return {};
    u16 group_type = 0;
    u16 group_count = 0;
    if (!read_u16(group, 2, group_type) || !read_u16(group, 4, group_count))
        return {};
    if (group_type != 1 || group_count == 0 || group_count > 64)
        return {};
    if (group.size() < 6 + static_cast<size_t>(group_count) * 14)
        return {};

    struct GroupEntry {
        u8 width, height, colors, reserved;
        u16 planes, bit_count;
        u32 bytes;
        u16 id;
        ReadonlyBytes image;
    };
    Vector<GroupEntry> images;
    for (u16 i = 0; i < group_count; ++i) {
        size_t offset = 6 + static_cast<size_t>(i) * 14;
        GroupEntry entry {};
        entry.width = group[offset];
        entry.height = group[offset + 1];
        entry.colors = group[offset + 2];
        entry.reserved = group[offset + 3];
        (void)read_u16(group, offset + 4, entry.planes);
        (void)read_u16(group, offset + 6, entry.bit_count);
        (void)read_u32(group, offset + 8, entry.bytes);
        (void)read_u16(group, offset + 12, entry.id);
        entry.image = resource_data(file, rsrc_offset, sections, 3, entry.id); // RT_ICON
        if (entry.image.is_empty())
            return {};
        images.append(entry);
    }

    // ICONDIR + ICONDIRENTRY[] + image blobs.
    push_u16(ico_storage, 0);
    push_u16(ico_storage, 1);
    push_u16(ico_storage, group_count);
    u32 data_offset = 6 + static_cast<u32>(group_count) * 16;
    for (auto const& entry : images) {
        u32 size = static_cast<u32>(entry.image.size());
        ico_storage.append(entry.width ? entry.width : 0);
        ico_storage.append(entry.height ? entry.height : 0);
        ico_storage.append(entry.colors);
        ico_storage.append(entry.reserved);
        push_u16(ico_storage, entry.planes);
        push_u16(ico_storage, entry.bit_count);
        push_u32(ico_storage, size);
        push_u32(ico_storage, data_offset);
        data_offset += size;
    }
    for (auto const& entry : images)
        push_bytes(ico_storage, entry.image);
    return ReadonlyBytes { ico_storage.data(), ico_storage.size() };
}

// omikuji-core's fallback: icons are frequently just a PNG blob or an embedded
// ICONDIR sitting in .rsrc; scanning for their signatures survives packers that
// confuse a strict resource-directory walk.
inline bool looks_like_icon_dir(ReadonlyBytes bytes)
{
    if (bytes.size() < 6)
        return false;
    u16 count = 0;
    if (!read_u16(bytes, 4, count) || count == 0 || count > 64)
        return false;
    size_t directory_length = 6 + static_cast<size_t>(count) * 16;
    if (bytes.size() < directory_length)
        return false;
    for (u16 i = 0; i < count; ++i) {
        size_t offset = 6 + static_cast<size_t>(i) * 16;
        u32 size = 0;
        u32 data_offset = 0;
        if (!read_u32(bytes, offset + 8, size) || !read_u32(bytes, offset + 12, data_offset))
            return false;
        if (size == 0 || data_offset < directory_length || static_cast<size_t>(data_offset) + size > bytes.size())
            return false;
    }
    return true;
}

inline bool plausible_icon_size(int width, int height)
{
    return width >= 16 && height >= 16 && width <= 512 && height <= 512;
}

inline RefPtr<Gfx::Bitmap const> icon_from_signature_scan(ReadonlyBytes file, int size)
{
    // Bounded: a 400 MB installer is not worth scanning on a UI thread.
    ReadonlyBytes bytes = file;
    constexpr size_t SCAN_LIMIT = 64 * 1024 * 1024;
    if (bytes.size() > SCAN_LIMIT)
        bytes = bytes.slice(0, SCAN_LIMIT);

    RefPtr<Gfx::Bitmap const> best;
    auto keep_largest = [&](RefPtr<Gfx::Bitmap const> candidate) {
        if (!candidate)
            return;
        if (!best || candidate->width() * candidate->height() > best->width() * best->height())
            best = move(candidate);
    };

    static constexpr u8 PNG_MAGIC[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    int png_attempts = 0;
    for (size_t i = 0; i + 8 <= bytes.size() && png_attempts < 64; ++i) {
        if (bytes[i] != PNG_MAGIC[0])
            continue;
        bool match = true;
        for (size_t k = 1; k < 8; ++k) {
            if (bytes[i + k] != PNG_MAGIC[k]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        ++png_attempts;
        auto slice = bytes.slice(i);
        if (!Gfx::PNGImageDecoderPlugin::sniff(slice))
            continue;
        auto decoder = Gfx::PNGImageDecoderPlugin::create(slice);
        if (decoder.is_error())
            continue;
        auto frame = decoder.value()->frame(0);
        if (frame.is_error() || !frame.value().image)
            continue;
        auto& image = *frame.value().image;
        if (!plausible_icon_size(image.width(), image.height()))
            continue;
        keep_largest(fit(image, size));
        i += 7;
    }

    int ico_attempts = 0;
    for (size_t i = 0; i + 4 <= bytes.size() && ico_attempts < 32; ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1 || bytes[i + 3] != 0)
            continue;
        auto slice = bytes.slice(i);
        if (!looks_like_icon_dir(slice))
            continue;
        ++ico_attempts;
        auto decoded = decode_bytes(slice, size);
        if (decoded && plausible_icon_size(size, size))
            keep_largest(move(decoded));
        i += 3;
    }
    return best;
}

inline bool is_windows_executable(StringView path)
{
    auto lower = ByteString { path }.to_lowercase();
    return lower.ends_with(".exe"sv) || lower.ends_with(".msi"sv) || lower.ends_with(".scr"sv);
}

inline RefPtr<Gfx::Bitmap const> from_executable(StringView path, int size)
{
    // A directory listing can ask for the same .exe on every repaint; the PE
    // scan is expensive, so cache by (path, size) including the failures.
    struct ExecutableCache {
        HashMap<ByteString, RefPtr<Gfx::Bitmap const>> bitmaps;
    };
    static ExecutableCache& cache = *new ExecutableCache;
    auto key = ByteString::formatted("exe:{}@{}", path, size);
    if (auto found = cache.bitmaps.find(key); found != cache.bitmaps.end())
        return found->value;

    RefPtr<Gfx::Bitmap const> result;
    auto mapped = Core::MappedFile::map(path);
    if (!mapped.is_error()) {
        auto file = mapped.value()->bytes();
        if (file.size() >= 0x40) {
            Vector<u8> ico_storage;
            auto ico = icon_from_resource_directory(file, ico_storage);
            if (!ico.is_empty())
                result = decode_bytes(ico, size);
            if (!result)
                result = icon_from_signature_scan(file, size);
        }
    }
    cache.bitmaps.set(key, result);
    return result;
}

// ---------------------------------------------------------------------------
// Public resolver

struct Cache {
    HashMap<ByteString, RefPtr<Gfx::Bitmap const>> bitmaps;
    HashMap<ByteString, ByteString> paths;
};

inline Cache& cache()
{
    static Cache instance;
    return instance;
}

inline void invalidate_cache()
{
    cache().bitmaps.clear();
    cache().paths.clear();
}

// Resolve any icon specification to a `size` x `size` bitmap.
// Accepted forms: absolute path, freedesktop theme name, path to a Windows
// executable, IconData=base64 payload.
inline RefPtr<Gfx::Bitmap const> resolve(StringView specification, int size)
{
    if (specification.is_empty() || size < 8)
        return {};
    ByteString spec { specification };
    auto key = ByteString::formatted("{}@{}", spec, size);
    auto& store = cache();
    if (auto it = store.bitmaps.find(key); it != store.bitmaps.end())
        return it->value;

    RefPtr<Gfx::Bitmap const> bitmap;
    if (spec.starts_with("base64:"sv)) {
        if (auto decoded = decode_base64(spec.substring_view(7)); !decoded.is_error())
            bitmap = decode_bytes(decoded.value(), size);
    } else if (is_windows_executable(spec)) {
        bitmap = from_executable(spec, size);
    } else {
        auto path = resolve_icon_file(spec, size);
        if (!path.is_empty()) {
            if (path.ends_with(".svg"sv)) {
                auto rasterized = rasterize_svg(path, size);
                if (!rasterized.is_empty())
                    bitmap = decode_file(rasterized, size);
            } else {
                bitmap = decode_file(path, size);
            }
        }
    }

    store.bitmaps.set(key, bitmap);
    return bitmap;
}

// Serenity application icons live in <prefix>/res/icons, not in an XDG theme
// directory, so the normal theme search never finds them. A native .af that
// omits Icon= still has a canonical icon named after its executable
// (/bin/ImageViewer -> app-image-viewer.png), which is what the old per-widget
// lookups guessed at. Do it once, here, for every caller.
inline ByteString serenity_app_icon_path(StringView executable, int size)
{
    auto name = ByteString { LexicalPath::basename(executable) };
    if (name.is_empty())
        return {};

    StringBuilder kebab;
    for (size_t i = 0; i < name.length(); ++i) {
        auto ch = name.characters()[i];
        if (i > 0 && ch >= 'A' && ch <= 'Z')
            kebab.append('-');
        kebab.append(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }

    ByteString root { "/res" };
    if (char const* prefix = getenv("SERENITY_PREFIX"); prefix && *prefix)
        root = ByteString::formatted("{}/res", prefix);

    for (auto directory_size : { size, 32 }) {
        // "TTPlayer" kebab-cases to "tt-player" but ships as app-ttplayer.png,
        // so try both spellings.
        for (auto const& candidate : { kebab.to_byte_string(), name.to_lowercase() }) {
            auto path = ByteString::formatted("{}/icons/{}x{}/app-{}.png", root, directory_size, directory_size, candidate);
            if (access(path.characters(), R_OK) == 0)
                return path;
        }
    }
    return {};
}

// Shortcut icon: the shortcut's own Icon= wins (that is what Linux apps use),
// falling back to the PE icon of the executable it launches, and finally to the
// app's own res icon when a native .af forgot to name one.
inline RefPtr<Gfx::Bitmap const> for_shortcut(StringView icon_specification, StringView executable, int size)
{
    if (auto bitmap = resolve(icon_specification, size))
        return bitmap;
    if (is_windows_executable(executable))
        return from_executable(executable, size);
    if (!executable.is_empty()) {
        if (auto path = serenity_app_icon_path(executable, size); !path.is_empty())
            return resolve(path, size);
    }
    return {};
}

// Resolved on-disk path for an icon spec (handy for properties dialogs).
inline ByteString resolved_path(StringView specification, int size)
{
    if (specification.is_empty())
        return {};
    ByteString spec { specification };
    auto key = ByteString::formatted("path:{}@{}", spec, size);
    auto& store = cache();
    if (auto it = store.paths.find(key); it != store.paths.end())
        return it->value;
    ByteString path;
    if (spec.starts_with('/'))
        path = resolve_icon_file(spec, size);
    else if (!is_windows_executable(spec) && !spec.starts_with("base64:"sv))
        path = resolve_icon_file(spec, size);
    if (path.ends_with(".svg"sv))
        path = rasterize_svg(path, size);
    store.paths.set(key, path);
    return path;
}

}
