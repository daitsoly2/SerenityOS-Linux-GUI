/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Shared archive operations for the desktop icon menu and the file manager, so
// "compress / extract" behaves (and is worded) the same in both places.

#pragma once

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Command.h>
#include <Ports/LinuxGUI/Common/hostify.h>

namespace PortArchiveOps {

inline bool is_archive(ByteString const& path)
{
    for (auto suffix : { ".zip"sv, ".7z"sv, ".rar"sv, ".tar"sv, ".tar.gz"sv, ".tgz"sv, ".tar.bz2"sv, ".tbz2"sv, ".tar.xz"sv, ".txz"sv }) {
        if (path.ends_with(suffix, CaseSensitivity::CaseInsensitive))
            return true;
    }
    return false;
}

// Run `tool args...`; returns an error message, or an empty string on success.
inline ByteString run_tool(ByteString const& tool, Vector<ByteString> const& arguments, ByteString const& workdir)
{
    auto result = Core::command(tool, arguments, LexicalPath { workdir });
    if (result.is_error())
        return ByteString::formatted("无法运行 {}", tool);
    if (result.value().exit_code != 0) {
        auto stderr_text = String::from_utf8_with_replacement_character(result.value().error).to_byte_string();
        if (stderr_text.is_empty())
            return ByteString::formatted("{} 退出码 {}", tool, result.value().exit_code);
        return ByteString::formatted("{} 退出码 {}\n{}", tool, result.value().exit_code, stderr_text);
    }
    return {};
}

// Compress `source` in place, next to itself. `output_stem` overrides the base
// file name of the result (without extension). Returns an error message or "".
inline ByteString compress(ByteString const& source, bool use_zip, Optional<ByteString> output_stem = {})
{
    auto directory = LexicalPath::dirname(source);
    auto input = LexicalPath::basename(source);
    auto stem = (output_stem.has_value() && !output_stem->is_empty()) ? *output_stem : input;

    if (use_zip) {
        auto output = ByteString::formatted("{}.zip", stem);
        if (auto sevenzip = PortHostify::find_tool({ "7z", "7za", "7zz" }); !sevenzip.is_empty())
            return run_tool(sevenzip, { "a", "-tzip", output, input }, directory);
        if (auto zip_tool = PortHostify::find_tool({ "zip" }); !zip_tool.is_empty())
            return run_tool(zip_tool, { "-r", output, input }, directory);
        return "找不到 zip 工具（请安装 7zip 或 zip）。";
    }

    if (auto tar_tool = PortHostify::find_tool({ "tar" }); !tar_tool.is_empty())
        return run_tool(tar_tool, { "czf", ByteString::formatted("{}.tar.gz", stem), input }, directory);
    return "找不到 tar 工具。";
}

// Extract `archive` into its own directory. Returns an error message or "".
inline ByteString extract(ByteString const& archive)
{
    auto directory = LexicalPath::dirname(archive);

    bool tar_suitable = false;
    for (auto suffix : { ".tar"sv, ".tar.gz"sv, ".tgz"sv, ".tar.bz2"sv, ".tbz2"sv, ".tar.xz"sv, ".txz"sv }) {
        if (archive.ends_with(suffix))
            tar_suitable = true;
    }

    if (tar_suitable) {
        if (auto tar_tool = PortHostify::find_tool({ "tar" }); !tar_tool.is_empty())
            return run_tool(tar_tool, { "xf", archive, "-C", "." }, directory);
    }
    if (auto sevenzip = PortHostify::find_tool({ "7z", "7za", "7zz" }); !sevenzip.is_empty())
        return run_tool(sevenzip, { "x", "-y", archive }, directory);
    if (!tar_suitable && archive.ends_with(".zip"sv)) {
        if (auto unzip_tool = PortHostify::find_tool({ "unzip" }); !unzip_tool.is_empty())
            return run_tool(unzip_tool, { "-o", archive }, directory);
    }
    return "找不到合适的解压工具。";
}

}
