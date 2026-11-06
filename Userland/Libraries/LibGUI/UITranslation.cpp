/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibGUI/UITranslation.h>
#include <stdlib.h>

namespace GUI {

StringView current_ui_language()
{
    static ByteString const s_language = [] {
        if (auto* env = getenv("SERENITY_LANGUAGE"); env && *env)
            return ByteString { env };

        auto config = Core::ConfigFile::open("/etc/serenity/session.conf");
        if (!config.is_error()) {
            auto value = config.value()->read_entry("Session", "Language");
            if (!value.is_empty())
                return value;
        }

        if (auto* env = getenv("LANG"); env && *env) {
            ByteString lang { env };
            if (auto dot = lang.find('.'); dot.has_value())
                lang = lang.substring(0, *dot);
            return lang.replace("_"sv, "-"sv, AK::ReplaceMode::All);
        }

        return ByteString { "en" };
    }();
    return s_language;
}

static HashMap<ByteString, ByteString> const& ui_table()
{
    static auto s_table = [] {
        HashMap<ByteString, ByteString> table;

        auto root = ByteString { "/res" };
        if (auto* env = getenv("SERENITY_RESOURCE_ROOT"); env && *env)
            root = ByteString { env };

        auto config = Core::ConfigFile::open(ByteString::formatted("{}/locale/{}.ini", root, current_ui_language()));
        if (config.is_error())
            return table;

        for (auto const& key : config.value()->keys("UI"sv))
            table.set(key, config.value()->read_entry("UI"sv, key));
        return table;
    }();
    return s_table;
}

size_t ui_translation_count()
{
    return ui_table().size();
}

namespace {

// Entries like "Score: {}" have to match the *formatted* result ("Score: 42"),
// because apps build those strings with String::formatted() before set_text().
struct PatternEntry {
    StringView prefix;
    StringView suffix;
    ByteString translated;
};

Vector<PatternEntry> const& ui_patterns()
{
    static auto s_patterns = [] {
        Vector<PatternEntry> patterns;
        for (auto const& entry : ui_table()) {
            auto position = entry.key.find("{}"sv);
            if (!position.has_value())
                continue;
            // Only single-placeholder entries: with two holes there is no way to
            // tell where the middle ends.
            if (entry.key.find("{}"sv, *position + 2).has_value())
                continue;
            patterns.append({ entry.key.substring_view(0, *position),
                entry.key.substring_view(*position + 2),
                entry.value });
        }
        // Longest prefix first, so "High Score: {}" wins over "Score: {}".
        quick_sort(patterns, [](auto& a, auto& b) { return a.prefix.length() > b.prefix.length(); });
        return patterns;
    }();
    return s_patterns;
}

}

ByteString translate_ui(ByteString const& text)
{
    if (text.is_empty())
        return text;

    auto const& table = ui_table();
    if (table.is_empty())
        return text;

    if (auto it = table.find(text); it != table.end())
        return it->value;

    // Menu entries carry mnemonics ("&File"); the table stores them without.
    if (text.contains('&')) {
        auto stripped = ByteString { text }.replace("&"sv, ""sv, AK::ReplaceMode::All);
        if (auto it = table.find(stripped); it != table.end())
            return it->value;
    }

    // Formatted status text: "Score: 42" against the table's "Score: {}".
    for (auto const& pattern : ui_patterns()) {
        if (text.length() <= pattern.prefix.length() + pattern.suffix.length())
            continue;
        if (!text.starts_with(pattern.prefix) || !text.ends_with(pattern.suffix))
            continue;

        auto middle = text.substring(pattern.prefix.length(), text.length() - pattern.prefix.length() - pattern.suffix.length());
        auto hole = pattern.translated.find("{}"sv);
        if (!hole.has_value())
            return pattern.translated;

        StringBuilder builder;
        builder.append(pattern.translated.substring_view(0, *hole));
        builder.append(middle);
        builder.append(pattern.translated.substring_view(*hole + 2));
        return builder.to_byte_string();
    }

    return text;
}

String translate_ui(String const& text)
{
    auto bytes = text.to_byte_string();
    auto translated = translate_ui(bytes);
    if (translated == bytes)
        return text;
    return String::from_utf8_with_replacement_character(translated);
}

}
