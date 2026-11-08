/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SkinDocument.h"
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCore/File.h>
#include <LibGfx/Bitmap.h>
#include <LibTextCodec/Decoder.h>

namespace {

// ---------------------------------------------------------------------------
// A deliberately small XML scanner. The original skin files use a permissive
// dialect (first-attribute lookup, no namespaces, GBK/UTF-8 mixed, stray
// comments), which is why TTPlayerRebuild also ships its own parser rather
// than a standards-compliant one. We only need tags, attributes and depth.
// ---------------------------------------------------------------------------

struct XmlAttribute {
    ByteString name;
    ByteString value;
};

struct XmlTag {
    ByteString name;
    Vector<XmlAttribute> attributes;
    int depth {};
    bool self_closing {};

    StringView attribute(StringView key) const
    {
        for (auto const& entry : attributes) {
            if (entry.name.view() == key)
                return entry.value.view();
        }
        return {};
    }
};

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
}

Vector<XmlTag> parse_tags(StringView text)
{
    Vector<XmlTag> tags;
    int depth = 0;
    size_t i = 0;
    while (i < text.length()) {
        auto lt = text.find('<', i);
        if (!lt.has_value())
            break;
        i = *lt;
        auto rest = text.substring_view(i);

        if (rest.starts_with("<!--"sv)) {
            auto end = text.find("-->"sv, i + 4);
            i = end.has_value() ? *end + 3 : text.length();
            continue;
        }
        if (rest.starts_with("<?"sv) || rest.starts_with("<!"sv)) {
            auto end = text.find('>', i);
            if (!end.has_value())
                break;
            i = *end + 1;
            continue;
        }
        if (rest.starts_with("</"sv)) {
            auto end = text.find('>', i);
            if (!end.has_value())
                break;
            --depth;
            i = *end + 1;
            continue;
        }

        size_t p = i + 1;
        while (p < text.length() && is_space(text[p]))
            ++p;
        size_t name_start = p;
        while (p < text.length() && is_name_char(text[p]))
            ++p;
        if (p == name_start) { // malformed; skip one byte
            ++i;
            continue;
        }

        XmlTag tag;
        tag.name = text.substring_view(name_start, p - name_start).to_byte_string();
        tag.depth = depth;

        while (p < text.length()) {
            while (p < text.length() && is_space(text[p]))
                ++p;
            if (p >= text.length())
                break;
            if (text[p] == '>') {
                ++p;
                break;
            }
            if (text[p] == '/' && p + 1 < text.length() && text[p + 1] == '>') {
                tag.self_closing = true;
                p += 2;
                break;
            }
            size_t attribute_start = p;
            while (p < text.length() && is_name_char(text[p]))
                ++p;
            if (p == attribute_start) {
                ++p;
                continue;
            }
            XmlAttribute attribute;
            attribute.name = text.substring_view(attribute_start, p - attribute_start).to_byte_string();
            while (p < text.length() && is_space(text[p]))
                ++p;
            if (p < text.length() && text[p] == '=') {
                ++p;
                while (p < text.length() && is_space(text[p]))
                    ++p;
                if (p < text.length() && (text[p] == '"' || text[p] == '\'')) {
                    char quote = text[p++];
                    size_t value_start = p;
                    while (p < text.length() && text[p] != quote)
                        ++p;
                    attribute.value = text.substring_view(value_start, p - value_start).to_byte_string();
                    if (p < text.length())
                        ++p;
                }
            }
            tag.attributes.append(move(attribute));
        }

        if (!tag.self_closing)
            ++depth;
        tags.append(move(tag));
        i = p;
    }
    return tags;
}

Optional<ByteString> read_text(StringView path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto const& bytes = contents.value();
    StringView raw { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    // Original .skn Skin.xml files are GBK-encoded. A stray non-UTF-8 byte
    // (notably the skin `name`) corrupts the skin-name IPC string and desyncs
    // the entire WindowServer frame, leaving every window black. Transparently
    // re-encode to UTF-8 so legacy packs render without manual conversion.
    if (!Utf8View { raw }.validate()) {
        if (auto decoder = TextCodec::decoder_for("GBK"sv); decoder.has_value()) {
            if (auto converted = decoder->to_utf8(raw); !converted.is_error())
                return converted.release_value().to_byte_string();
        }
    }
    return raw.to_byte_string();
}

// Original .skn packs are 8-bit *palette* BMPs (Classic.skn: 256-colour
// table, data offset 1078). LibGfx decodes them to an indexed bitmap, which
// Painter::blit() cannot composite correctly onto our BGRA8888 backing store
// - the whole panel comes out black. Always materialise a plain RGBA bitmap.
RefPtr<Gfx::Bitmap> to_rgba(Gfx::Bitmap const& source)
{
    auto maybe_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, source.size());
    if (maybe_bitmap.is_error())
        return nullptr;
    auto destination = maybe_bitmap.release_value();
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x)
            destination->set_pixel(x, y, source.get_pixel(x, y));
    }
    return destination;
}

RefPtr<Gfx::Bitmap> load_bitmap(StringView directory, StringView file)
{
    if (file.is_empty())
        return nullptr;
    StringBuilder path;
    path.append(directory);
    path.append('/');
    for (char c : file)
        path.append(c == '\\' ? '/' : c);
    auto normalized = path.to_byte_string();
    auto bitmap = Gfx::Bitmap::load_from_file(normalized.view());
    if (!bitmap.is_error()) {
        if (auto converted = to_rgba(*bitmap.value()))
            return converted;
        return bitmap.value();
    }
    // Some packs reference "images\\foo.bmp" but ship the file flattened.
    auto flat = ByteString::formatted("{}/{}", directory, LexicalPath::basename(normalized.view()));
    auto retry = Gfx::Bitmap::load_from_file(flat.view());
    if (!retry.is_error()) {
        if (auto converted = to_rgba(*retry.value()))
            return converted;
        return retry.value();
    }
    return nullptr;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Optional<Gfx::Color> parse_color(StringView value)
{
    value = value.trim_whitespace();
    // Native skins also allow "255,0,255" for the mask colour.
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
    if (!value.starts_with("#"sv) || value.length() < 7)
        return {};
    int channel[3];
    for (size_t i = 0; i < 3; ++i) {
        int high = hex_value(value[1 + i * 2]);
        int low = hex_value(value[2 + i * 2]);
        if (high < 0 || low < 0)
            return {};
        channel[i] = high * 16 + low;
    }
    return Gfx::Color::from_rgb((channel[0] << 16) | (channel[1] << 8) | channel[2]);
}

// Native `position` / `resize_rect` are two points: left,top,right,bottom.
Gfx::IntRect parse_position(StringView value)
{
    auto parts = value.split_view(',');
    if (parts.size() != 4)
        return {};
    int v[4];
    for (size_t i = 0; i < 4; ++i)
        v[i] = parts[i].trim_whitespace().to_number<int>().value_or(0);
    if (v[2] <= v[0] || v[3] <= v[1])
        return {};
    return Gfx::IntRect { v[0], v[1], v[2] - v[0], v[3] - v[1] };
}

unsigned parse_alignment(StringView value, unsigned initial = 0)
{
    auto apply = [](unsigned result, StringView token) {
        if (token == "left"sv) return (result & 0xf0U) | 1U;
        if (token == "center"sv) return (result & 0xf0U) | 2U;
        if (token == "right"sv) return (result & 0xf0U) | 3U;
        if (token == "top"sv) return (result & 0x0fU) | 0x10U;
        if (token == "vcenter"sv) return (result & 0x0fU) | 0x20U;
        if (token == "bottom"sv) return (result & 0x0fU) | 0x30U;
        return result;
    };
    unsigned result = initial;
    auto plus = value.find('+');
    result = apply(result, value.substring_view(0, plus.value_or(value.length())).trim_whitespace());
    if (plus.has_value())
        result = apply(result, value.substring_view(*plus + 1).trim_whitespace());
    return result;
}

// Front-to-back child order from TTPlayerRebuild's PlayerSkinOrder().
// Unknown XML elements have no native slot.
int player_skin_order(StringView name)
{
    constexpr StringView names[] = {
        "exit"sv, "minimize"sv, "minimode"sv, "equalizer"sv, "lyric"sv, "playlist"sv,
        "browser"sv, "play"sv, "pause"sv, "stop"sv, "prev"sv, "next"sv, "open"sv, "mute"sv,
        "icon"sv, "info"sv, "visual"sv, "led"sv, "stereo"sv, "status"sv, "progress"sv, "volume"sv,
        "set"sv, "login"sv, "login_name"sv, "mode_single"sv, "mode_loop"sv, "mode_slider"sv,
        "mode_circle"sv, "mode_random"sv
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (names[i] == name)
            return static_cast<int>(i);
    }
    return -1;
}

bool is_four_state_button(StringView name)
{
    constexpr StringView names[] = {
        "play"sv, "pause"sv, "stop"sv, "prev"sv, "next"sv, "mute"sv, "open"sv,
        "lyric"sv, "equalizer"sv, "playlist"sv, "browser"sv, "exit"sv,
        "minimize"sv, "minimode"sv, "set"sv, "login"sv, "mode_single"sv, "mode_loop"sv,
        "mode_slider"sv, "mode_circle"sv, "mode_random"sv,
        // Popup-window buttons use the same four-frame convention.
        "close"sv, "ontop"sv, "desklrc"sv, "enabled"sv
    };
    for (auto const& entry : names) {
        if (entry == name)
            return true;
    }
    return false;
}

bool is_text_element(StringView name)
{
    return name == "icon"sv || name == "info"sv || name == "stereo"sv
        || name == "status"sv || name == "login_name"sv;
}

SkinElement parse_element(StringView directory, XmlTag const& tag)
{
    SkinElement element;
    element.name = tag.name;
    element.bounds = parse_position(tag.attribute("position"sv));
    element.color = parse_color(tag.attribute("color"sv)).value_or(Gfx::Color { 255, 255, 255 });
    element.background = parse_color(tag.attribute("bkgnd"sv)).value_or(Gfx::Color { 0, 0, 0 });
    element.alignment = is_text_element(tag.name.view())
        ? parse_alignment(tag.attribute("align"sv), 1)
        : ((tag.name.view() == "led"sv || tag.name.view() == "visual"sv)
                ? parse_alignment(tag.attribute("align"sv))
                : 0);
    auto font = tag.attribute("font"sv);
    if (!font.is_empty()) {
        element.font = font.to_byte_string();
        element.font_size = tag.attribute("font_size"sv).to_number<int>().value_or(12);
    }
    // `vertical="true"` is what the format documents, but classic skins leave
    // the attribute off their EQ faders entirely: <preamp position="13,74,31,154"
    // thumb_image="eq_thumb.bmp"/>. A groove that is far taller than it is wide
    // can only be a vertical slider, and without this the faders were dragged
    // left/right and their grips slid sideways along the 0 dB line.
    auto vertical_attribute = tag.attribute("vertical"sv);
    element.vertical = vertical_attribute == "true"sv;
    if (vertical_attribute.is_empty() && element.bounds.height() > element.bounds.width() * 2)
        element.vertical = true;

    element.image = load_bitmap(directory, tag.attribute("image"sv));
    element.bar_image = load_bitmap(directory, tag.attribute("bar_image"sv));
    element.fill_image = load_bitmap(directory, tag.attribute("fill_image"sv));
    element.fill_image2 = load_bitmap(directory, tag.attribute("fill_image2"sv));
    element.thumb_image = load_bitmap(directory, tag.attribute("thumb_image"sv));
    element.flash_image = load_bitmap(directory, tag.attribute("flash_image"sv));
    element.flash_mode = tag.attribute("flash_mode"sv).to_number<int>().value_or(0);
    element.frame_count = tag.attribute("frame_count"sv).to_number<int>().value_or(10);
    element.frame_interval = tag.attribute("frame_interval"sv).to_number<int>().value_or(100);

    element.four_state = is_four_state_button(tag.name.view());
    if (element.image && !element.bounds.is_empty()) {
        if (tag.name.view() == "led"sv) {
            // A LED strip is always 12 equally-wide glyphs: 0-9, ':' and '-'.
            // Skins do not declare the frame count anywhere.
            element.frames = 12;
        } else if (element.four_state && element.bounds.width() > 0
            && element.image->width() % element.bounds.width() == 0) {
            // Frame width comes from `position` (right-left), NOT a hard-coded
            // four: some packs ship 2/3-state sprites. Verified against real
            // Classic.skn: play.bmp 120x30 with position width 30 -> 4 frames,
            // stop.bmp 80x20 with position width 20 -> 4 frames.
            element.frames = element.image->width() / element.bounds.width();
        }
        // Keep the XML rectangle as the hit target; it is authoritative.
    }
    return element;
}

}

Gfx::IntSize SkinElement::frame_size() const
{
    if (!image || frames <= 0)
        return {};
    return { image->width() / frames, image->height() };
}

Gfx::IntRect SkinElement::frame_source_rect(int state) const
{
    auto size = frame_size();
    if (size.is_empty())
        return {};
    int index = state < 0 ? 0 : (state >= frames ? frames - 1 : state);
    return { index * size.width(), 0, size.width(), size.height() };
}

SkinDocument SkinDocument::load(StringView directory)
{
    SkinDocument document;
    auto manifest_path = ByteString::formatted("{}/Skin.xml", directory);
    auto text = read_text(manifest_path.view());
    if (!text.has_value()) {
        manifest_path = ByteString::formatted("{}/skin.xml", directory);
        text = read_text(manifest_path.view());
    }
    if (!text.has_value())
        return document;

    auto tags = parse_tags(text->view());
    if (tags.is_empty())
        return document;

    auto const& root = tags.first();
    // The original parser rejects anything but version 2.
    if (root.attribute("version"sv).to_number<int>().value_or(0) != 2)
        return document;

    document.transparent_color = parse_color(root.attribute("transparent_color"sv));
    auto name = root.attribute("name"sv);
    document.name = name.is_empty() ? LexicalPath::basename(directory) : name.to_byte_string();
    document.author = root.attribute("author"sv).to_byte_string();

    ByteString main_image_attr;

    for (size_t i = 0; i < tags.size(); ++i) {
        auto const& tag = tags[i];
        if (tag.depth != 1)
            continue;
        auto tag_name = tag.name.view();
        SkinPanelLayout* panel = nullptr;
        bool is_player = false;
        if (tag_name == "player_window"sv) {
            panel = &document.main;
            is_player = true;
            main_image_attr = tag.attribute("image"sv).to_byte_string();
        } else if (tag_name == "playlist_window"sv) {
            panel = &document.playlist;
        } else if (tag_name == "lyric_window"sv) {
            panel = &document.lyrics;
        } else if (tag_name == "equalizer_window"sv) {
            panel = &document.equalizer;
        } else {
            continue;
        }

        panel->position = parse_position(tag.attribute("position"sv));
        panel->resize_rect = parse_position(tag.attribute("resize_rect"sv));
        panel->resize_tile = tag.attribute("resize_tile"sv).to_number<int>().value_or(0) != 0;
        if (tag_name == "equalizer_window"sv)
            panel->eq_interval = tag.attribute("eq_interval"sv).to_number<int>().value_or(0);
        panel->background = load_bitmap(directory, tag.attribute("image"sv));

        // Everything up to the next depth<=1 tag belongs to this window.
        // Keeping document order matches the native front-to-back slot order
        // for the vast majority of packs (controls rarely overlap).
        for (size_t j = i + 1; j < tags.size() && tags[j].depth >= 2; ++j) {
            if (tags[j].depth != 2)
                continue;
            if (is_player && player_skin_order(tags[j].name.view()) < 0)
                continue;
            panel->elements.append(parse_element(directory, tags[j]));
        }
        panel->valid = panel->background || !panel->elements.is_empty();
    }

    // Every panel's control bitmaps share the document-wide mask colour.
    if (document.transparent_color.has_value()) {
        for (auto* panel : { &document.main, &document.playlist, &document.lyrics, &document.equalizer })
            panel->transparent_color = document.transparent_color;
    }

    // A native equalizer declares a single <eqfactor> slider; the player is
    // expected to lay out one copy per frequency band, spaced by `eq_interval`
    // pixels. Without this the window showed two thumbs (preamp + the one
    // literal eqfactor) and eight bare grooves.
    if (document.equalizer.valid) {
        Vector<SkinElement> expanded;
        for (auto& element : document.equalizer.elements) {
            if (element.name != "eqfactor") {
                expanded.append(move(element));
                continue;
            }
            int spacing = element.bounds.width() + document.equalizer.eq_interval;
            if (spacing <= 0)
                spacing = element.bounds.width();
            for (size_t band = 0; band < 10; ++band) {
                auto copy = element;
                copy.name = ByteString::formatted("eqfactor{}", band);
                copy.bounds.translate_by(static_cast<int>(band) * spacing, 0);
                expanded.append(move(copy));
            }
        }
        document.equalizer.elements = move(expanded);
    }

    document.valid = document.main.valid || document.playlist.valid
        || document.lyrics.valid || document.equalizer.valid;
    dbgln("SkinDoc {}: name={} valid={} main(bg={} elems={} pl(bg={} elems={}) ly(bg={} eq(bg={}) img_attr=[{}])",
        directory, document.name, document.valid,
        document.main.background ? 1 : 0, document.main.elements.size(),
        document.playlist.background ? 1 : 0, document.playlist.elements.size(),
        document.lyrics.background ? 1 : 0, document.equalizer.background ? 1 : 0,
        main_image_attr);
    return document;
}
