/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StartMenuOverrides.h"
#include <AK/ByteString.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>

namespace StartMenuOverrides {

static RefPtr<Core::ConfigFile> s_config;

ByteString config_path()
{
    return ByteString::formatted("{}/start-menu-overrides.ini", Core::StandardPaths::config_directory());
}

static Core::ConfigFile* config()
{
    if (s_config)
        return s_config.ptr();

    auto dir = Core::StandardPaths::config_directory();
    (void)Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes);

    auto path = config_path();
    // ConfigFile::open(..., AllowWriting::Yes) needs an existing file; create
    // the empty document on first use.
    if (!FileSystem::exists(path)) {
        auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
        if (file.is_error()) {
            warnln("start menu: cannot create {}: {}", path, file.error());
            return nullptr;
        }
    }

    auto opened = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
    if (opened.is_error()) {
        warnln("start menu: cannot open {}: {}", path, opened.error());
        return nullptr;
    }
    s_config = opened.release_value();
    return s_config.ptr();
}

static void save(Core::ConfigFile& cfg)
{
    if (auto result = cfg.sync(); result.is_error())
        warnln("start menu: cannot write overrides: {}", result.error());
}

Override get(ByteString const& key)
{
    auto* cfg = config();
    if (!cfg)
        return {};
    return {
        .hidden = cfg->read_bool_entry(key, "Hidden", false),
        .category = cfg->read_entry(key, "Category").trim_whitespace(),
    };
}

void set_hidden(ByteString const& key, bool hidden)
{
    auto* cfg = config();
    if (!cfg)
        return;
    if (hidden)
        cfg->write_entry(key, "Hidden", "1");
    else
        cfg->remove_entry(key, "Hidden");
    save(*cfg);
}

void set_category(ByteString const& key, ByteString const& category)
{
    auto* cfg = config();
    if (!cfg)
        return;
    if (category.is_empty())
        cfg->remove_entry(key, "Category");
    else
        cfg->write_entry(key, "Category", category);
    save(*cfg);
}

void clear(ByteString const& key)
{
    auto* cfg = config();
    if (!cfg)
        return;
    cfg->remove_entry(key, "Hidden");
    cfg->remove_entry(key, "Category");
    save(*cfg);
}

}
