/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace GUI {

// Runtime UI string table.
//
// Serenity apps hardcode English labels; upstream's LibLocale::Translations does
// not exist in this tree, and patching 78 apps is not practical. Instead the
// widget text setters (buttons, labels, menus, window titles, group boxes, tabs,
// actions) look every string up in /res/locale/<lang>.ini under [UI] and fall
// back to the original text.
//
// Only exact matches are translated, so file names, user input, formatted
// messages ("Copied 3 files") and anything not in the table pass through
// unchanged. Menus are written without '&' mnemonics in the table; a lookup
// with the '&' stripped is attempted as well.
StringView current_ui_language();
size_t ui_translation_count();

ByteString translate_ui(ByteString const& text);
String translate_ui(String const& text);

}
