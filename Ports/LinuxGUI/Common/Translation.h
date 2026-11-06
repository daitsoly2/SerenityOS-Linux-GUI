/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/StringView.h>
#include <LibCore/ConfigFile.h>
#include <stdlib.h>

// Minimal UI translation layer for the Linux GUI port.
//
// This tree predates upstream's LibLocale::Translations, and pulling that in
// would mean touching every app. Instead we ship a flat catalog per language at
// /res/locale/<lang>.ini and translate the strings we actually own (desktop
// shell, login, port-specific dialogs) at runtime:
//
//   [Menu]
//   Settings=设置
//
// Language selection: [Session] Language= in /etc/serenity/session.conf, else
// $LANG (zh_CN.UTF-8 -> zh-CN), else English.

namespace PortTranslation {

inline ByteString configured_language()
{
    if (auto* lang = getenv("SERENITY_LANGUAGE"); lang && *lang)
        return ByteString { lang };

    auto config = Core::ConfigFile::open(ByteString { "/etc/serenity/session.conf" });
    if (!config.is_error()) {
        auto value = config.value()->read_entry("Session", "Language");
        if (!value.is_empty())
            return value;
    }

    if (auto* env = getenv("LANG"); env && *env) {
        ByteString lang { env };
        auto dot = lang.find('.');
        if (dot.has_value())
            lang = lang.substring(0, *dot);
        return lang.replace("_"sv, "-"sv, AK::ReplaceMode::All);
    }
    return ByteString { "en" };
}

inline HashMap<ByteString, ByteString> const& catalog()
{
    static HashMap<ByteString, ByteString> s_catalog = [] {
        HashMap<ByteString, ByteString> map;
        auto language = configured_language();
        auto root = ByteString { "/res" };
        if (auto* env = getenv("SERENITY_RESOURCE_ROOT"); env && *env)
            root = ByteString { env };
        auto path = ByteString::formatted("{}/locale/{}.ini", root, language);
        auto config = Core::ConfigFile::open(path);
        if (config.is_error())
            return map;
        for (auto const& group : config.value()->groups()) {
            for (auto const& key : config.value()->keys(group)) {
                map.set(ByteString::formatted("{}.{}", group, key), config.value()->read_entry(group, key));
            }
        }
        return map;
    }();
    return s_catalog;
}

// Look up "<group>.<key>"; fall back to the English source string.
inline ByteString tr(StringView group, StringView key, StringView fallback)
{
    auto lookup = ByteString::formatted("{}.{}", group, key);
    if (auto it = catalog().find(lookup); it != catalog().end())
        return it->value;
    return ByteString { fallback };
}

} // namespace PortTranslation
