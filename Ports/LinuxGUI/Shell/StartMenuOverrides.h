/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>

namespace StartMenuOverrides {

// Per-entry overrides for start-menu items whose source files are read-only
// (/res/apps/*.af and the XDG .desktop launchers). Stored as an INI file in
// the user's config directory, keyed by a stable entry key (the source path).
struct Override {
    bool hidden { false };
    ByteString category; // empty = keep the source file's category
};

ByteString config_path();
Override get(ByteString const& key);
void set_hidden(ByteString const& key, bool hidden);
void set_category(ByteString const& key, ByteString const& category);
void clear(ByteString const& key);

}
