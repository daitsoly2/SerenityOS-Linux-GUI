/*
 * serenity-ime-settings -- LibGUI control-panel applet for the Serenity IME.
 *
 * This is the front end for the configuration that the fcitx5 `serenityime`
 * addon consumes. It writes rime's custom YAML directly (there is no rime
 * config library in the port), deploys it with rime_deployer, then asks the
 * addon over its control socket to reload the rime addon and re-activate the
 * selected input method.
 *
 * The three product requirements baked into this applet:
 *   1. Rime is the ONLY Chinese input method. The profile we emit contains
 *      exactly `keyboard-us` (English) and `rime` (Chinese); there is no UI to
 *      add another Chinese engine, and no other engine is ever written back.
 *   2. Only the common fuzzy-pinyin groups are offered. f/h and g/k confusion
 *      are deliberately NOT options (and are never generated into the YAML).
 *   3. The candidate preview shows 9 entries by default, using the very same
 *      GUI::InputMethodPanelWidget that the real candidate window uses.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/GroupBox.h>
#include <LibGUI/InputMethodCandidateWindow.h>
#include <LibGUI/Label.h>
#include <LibGUI/SettingsWindow.h>
#include <LibGUI/SpinBox.h>
#include <LibGUI/Widget.h>
#include <LibMain/Main.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct IMESettings {
    bool enabled { true };             // DefaultIM=rime (false -> keyboard-us)
    bool zh_z { true };                // z/zh, c/ch, s/sh
    bool n_l { true };                 // n/l
    bool r_l { true };                 // r/l
    bool en_eng { true };              // en/eng, in/ing
    bool eng_ong { true };             // eng/ong after b,p,m,f
    bool abbreviation { true };        // 简拼
    bool spelling_correction { true };
    bool key_correction { true };
    int page_size { 9 };
};

// NOTE: there is intentionally no f/h and no g/k member. Those two confusion
// pairs were explicitly excluded from the product requirements.
struct FuzzyOption {
    char const* fragment;
    bool IMESettings::* flag;
    StringView label;
};

FuzzyOption const s_fuzzy_options[] = {
    { "pinyin:/zh_z_bufen", &IMESettings::zh_z, "z / zh、c / ch、s / sh  平翘舌"sv },
    { "pinyin:/n_l_bufen", &IMESettings::n_l, "n / l  鼻音与边音"sv },
    { "pinyin:/r_l_bufen", &IMESettings::r_l, "r / l  卷舌与边音"sv },
    { "pinyin:/en_eng_bufen", &IMESettings::en_eng, "en / eng、in / ing  前后鼻音"sv },
    { "pinyin:/eng_ong_bufen", &IMESettings::eng_ong, "eng / ong  b、p、m、f 之后"sv },
    { "pinyin:/abbreviation", &IMESettings::abbreviation, "简拼（首字母输入）"sv },
    { "pinyin:/spelling_correction", &IMESettings::spelling_correction, "拼写纠错"sv },
    { "pinyin:/key_correction", &IMESettings::key_correction, "按键纠错"sv },
};
size_t const s_fuzzy_option_count = sizeof(s_fuzzy_options) / sizeof(s_fuzzy_options[0]);

IMESettings s_settings;
ByteString s_last_result;

// ---------------------------------------------------------------- paths ----

ByteString home_directory()
{
    if (char const* value = getenv("HOME"); value && *value)
        return value;
    return "/home/serenity";
}

ByteString rime_user_directory()
{
    if (char const* value = getenv("SERENITY_IME_USER_DIR"); value && *value)
        return value;
    return ByteString::formatted("{}/.local/share/fcitx5/rime", home_directory().characters());
}

ByteString luna_custom_path()
{
    return ByteString::formatted("{}/luna_pinyin.custom.yaml", rime_user_directory().characters());
}

ByteString default_custom_path()
{
    return ByteString::formatted("{}/default.custom.yaml", rime_user_directory().characters());
}

ByteString profile_path()
{
    return ByteString::formatted("{}/.config/fcitx5/profile", home_directory().characters());
}

ByteString control_socket_path()
{
    char const* dir = getenv("SERENITY_IME_STATUS_DIR");
    return ByteString::formatted("{}/serenity-ime-ctl.sock", (dir && *dir) ? dir : "/tmp");
}

// ------------------------------------------------------------- file I/O ----

ByteString read_whole_file(ByteString const& path)
{
    FILE* file = fopen(path.characters(), "rb");
    if (!file)
        return {};
    StringBuilder builder;
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        builder.append(StringView { buffer, count });
    fclose(file);
    return builder.to_byte_string();
}

bool write_whole_file(ByteString const& path, StringView content)
{
    FILE* file = fopen(path.characters(), "wb");
    if (!file)
        return false;
    bool ok = fwrite(content.characters_without_null_termination(), 1, content.length(), file) == content.length();
    fclose(file);
    return ok;
}

// The generated YAML is the source of truth: read it back so the applet always
// shows what rime will actually use, even if someone edited the file by hand.
void load_from_disk()
{
    auto custom = read_whole_file(luna_custom_path());
    if (!custom.is_empty()) {
        char const* text = custom.characters();
        for (auto const& option : s_fuzzy_options)
            s_settings.*(option.flag) = strstr(text, option.fragment) != nullptr;

        s_settings.page_size = 9;
        if (char const* page = strstr(text, "menu/page_size:"))
            s_settings.page_size = atoi(page + 15);
        if (s_settings.page_size < 5 || s_settings.page_size > 10)
            s_settings.page_size = 9;
    }

    auto profile = read_whole_file(profile_path());
    if (!profile.is_empty())
        s_settings.enabled = strstr(profile.characters(), "DefaultIM=rime") != nullptr;
}

void reset_to_defaults()
{
    s_settings = IMESettings {};
}

// ------------------------------------------------------- external actions --

// rime_deployer --build is a no-op while fcitx5 holds the user data dir: it
// exits 0 but leaves build/*.yaml untouched, so custom YAML changes never take
// effect. fcitx5's own controller has a supported Restart method: the new
// process re-deploys rime on startup (rebuilding build/ from the custom YAML)
// and re-creates the IME socket. CanRestart reports true on this image.
// This is also much faster than a full deploy, so Apply does not freeze the UI.
void send_control_command(StringView command); // defined below

// fcitx5 saves its profile from *in-memory* state on a graceful exit, and
// Controller1.Restart is exactly that. SetCurrentIM does not update the
// group's DefaultIM, so writing the profile before the restart is pointless:
// the exiting process clobbers it. Wait until the new fcitx5 has started and
// its exit-save has landed, then rewrite the profile once more and switch the
// live input method over the control socket. Double-fork so the GUI process
// never has to reap a child that sleeps for several seconds.
void schedule_finalize(ByteString const& profile, bool enabled)
{
    pid_t child = fork();
    if (child < 0)
        return;
    if (child == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            sleep(4);
            write_whole_file(profile_path(), profile.view());
            send_control_command(enabled ? "set rime"sv : "toggle off"sv);
            _exit(0);
        }
        _exit(0);
    }
    int status = 0;
    (void)waitpid(child, &status, 0); // reap the short-lived intermediate child
}

bool restart_input_method()
{
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("busctl", "busctl", "--user", "call",
            "org.fcitx.Fcitx5", "/controller",
            "org.fcitx.Fcitx.Controller1", "Restart", nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void send_control_command(StringView command)
{
    auto path = control_socket_path();
    if (path.length() >= sizeof(sockaddr_un::sun_path))
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.characters(), path.length());
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return;
    }
    auto line = ByteString::formatted("{}\n", command);
    (void)send(fd, line.characters(), line.length(), MSG_NOSIGNAL);
    close(fd);
}

ByteString apply_to_disk()
{
    StringBuilder yaml;
    yaml.append("# Generated by serenity-ime-settings\n"sv);
    yaml.append("# Serenity 输入法：常见模糊音（明确排除 f/h 与 g/k）\n"sv);
    yaml.append("patch:\n"sv);

    size_t enabled_fuzzy = 0;
    for (auto const& option : s_fuzzy_options) {
        if (s_settings.*(option.flag))
            ++enabled_fuzzy;
    }
    if (enabled_fuzzy > 0) {
        yaml.append("  speller/algebra:\n"sv);
        yaml.append("    __patch:\n"sv);
        for (auto const& option : s_fuzzy_options) {
            if (!(s_settings.*(option.flag)))
                continue;
            // ByteString::view() is lvalue-only; keep the formatted line alive.
            auto line = ByteString::formatted("      - {}        # {}\n",
                option.fragment, option.label);
            yaml.append(line.view());
        }
    }
    auto page_line = ByteString::formatted("  menu/page_size: {}\n", s_settings.page_size);
    yaml.append(page_line.view());

    bool wrote_yaml = write_whole_file(luna_custom_path(), yaml.string_view());
    auto default_custom = ByteString::formatted("patch:\n  menu/page_size: {}\n", s_settings.page_size);
    bool wrote_default = write_whole_file(default_custom_path(), default_custom.view());

    // Only two input methods, ever: English keyboard + rime. No pinyin, no
    // bopomofo, no cangjie. fcitx5 rewrites this file on exit, so we also
    // switch the live session through the control socket below.
    auto profile = ByteString::formatted(
        "[Groups/0]\n"
        "Name=Default\n"
        "Default Layout=us\n"
        "DefaultIM={}\n"
        "[Groups/0/Items/0]\n"
        "Name=keyboard-us\n"
        "Layout=\n"
        "[Groups/0/Items/1]\n"
        "Name=rime\n"
        "Layout=\n"
        "[GroupOrder]\n"
        "0=Default\n",
        s_settings.enabled ? "rime" : "keyboard-us");
    bool wrote_profile = write_whole_file(profile_path(), profile.view());

    // The profile already carries the requested DefaultIM, so the restarting
    // fcitx5 comes up with rime (or the plain keyboard) as selected. No control
    // socket command is needed: the addon is reloaded with the process.
    bool restarted = restart_input_method();
    // Always finalize after the restart: it makes DefaultIM stick and switches
    // the live session even when the DBus controller is unavailable.
    schedule_finalize(profile, s_settings.enabled);
    if (!restarted) {
        // Fallback for images without the DBus controller: at least ask the
        // running addon to reload its config and re-activate the engine.
        send_control_command(s_settings.enabled ? "reload 1"sv : "reload 0"sv);
    }

    return ByteString::formatted(
        "已写入 · 模糊音 {}/{} 组 · 候选 {} 个 · {} · {}",
        enabled_fuzzy, s_fuzzy_option_count, s_settings.page_size,
        restarted ? "输入法已重启并重新部署（立即生效）" : "已通知输入法重载（可能需要重新登录）",
        (!wrote_yaml || !wrote_default || !wrote_profile) ? "部分文件不可写" : "仅 Rime");
}

String as_string(ByteString const& value)
{
    return String::from_byte_string(value).release_value_but_fixme_should_propagate_errors();
}

// ------------------------------------------------------------- the tab -----

class IMESettingsTab final : public GUI::SettingsWindow::Tab {
    C_OBJECT_ABSTRACT(IMESettingsTab)

public:
    static ErrorOr<NonnullRefPtr<IMESettingsTab>> try_create();
    void refresh_controls();

private:
    IMESettingsTab() = default;
    ErrorOr<void> setup();
    void rebuild_preview();

    virtual void apply_settings() override;
    virtual void cancel_settings() override;
    virtual void reset_default_values() override;

    RefPtr<GUI::CheckBox> m_enabled_checkbox;
    Vector<RefPtr<GUI::CheckBox>> m_fuzzy_checkboxes;
    RefPtr<GUI::SpinBox> m_page_size_spinbox;
    RefPtr<GUI::InputMethodPanelWidget> m_preview;
    RefPtr<GUI::Label> m_status_label;
};

ErrorOr<NonnullRefPtr<IMESettingsTab>> IMESettingsTab::try_create()
{
    auto tab = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) IMESettingsTab()));
    TRY(tab->setup());
    return tab;
}

ErrorOr<void> IMESettingsTab::setup()
{
    load_from_disk();

    set_layout<GUI::VerticalBoxLayout>(6, 8);
    // Root widget must paint its background, otherwise the transparent Label
    // areas leave black holes in the window buffer.
    set_fill_with_background_color(true);

    // --- 1. Chinese input method: rime only -------------------------------
    auto& general = add<GUI::GroupBox>("中文输入法"sv);
    general.set_layout<GUI::VerticalBoxLayout>(3, 4);
    auto& scheme_label = general.add<GUI::Label>("当前方案：Rime · 朙月拼音（唯一中文输入法）"_string);
    scheme_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    scheme_label.set_fixed_height(20);
    auto& scheme_note = general.add<GUI::Label>("已移除 pinyin / bopomofo / cangjie 等其它方案，只保留英文键盘与 Rime。"_string);
    scheme_note.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    scheme_note.set_fixed_height(20);
    m_enabled_checkbox = general.add<GUI::CheckBox>("启用中文输入法（关闭则仅英文键盘）"_string);
    m_enabled_checkbox->set_fixed_height(22);
    m_enabled_checkbox->on_checked = [this](bool checked) {
        s_settings.enabled = checked;
        set_modified(true);
    };

    // --- 2. Fuzzy pinyin (no f/h, no g/k) ---------------------------------
    auto& fuzzy = add<GUI::GroupBox>("常见模糊音"sv);
    fuzzy.set_layout<GUI::VerticalBoxLayout>(2, 4);
    for (size_t i = 0; i < s_fuzzy_option_count; ++i) {
        auto& checkbox = fuzzy.add<GUI::CheckBox>(
            String::from_utf8(s_fuzzy_options[i].label).release_value_but_fixme_should_propagate_errors());
        checkbox.set_fixed_height(20);
        checkbox.on_checked = [this, i](bool checked) {
            s_settings.*(s_fuzzy_options[i].flag) = checked;
            set_modified(true);
        };
        m_fuzzy_checkboxes.append(checkbox);
    }
    auto& excluded = fuzzy.add<GUI::Label>("已按要求排除：f / h 混淆、g / k 混淆（不提供、不生成规则）"_string);
    excluded.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    excluded.set_fixed_height(20);

    // --- 3. Candidate count + live 9-item preview -------------------------
    auto& candidates = add<GUI::GroupBox>("候选预览"sv);
    candidates.set_layout<GUI::VerticalBoxLayout>(3, 4);
    auto& spin_row = candidates.add<GUI::Widget>();
    spin_row.set_layout<GUI::HorizontalBoxLayout>(6);
    spin_row.set_fixed_height(26);
    auto& spin_label = spin_row.add<GUI::Label>("每页候选数："_string);
    spin_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    spin_label.set_fixed_width(110);
    m_page_size_spinbox = spin_row.add<GUI::SpinBox>();
    m_page_size_spinbox->set_fixed_width(80);
    m_page_size_spinbox->set_range(5, 10, GUI::AllowCallback::No);
    m_page_size_spinbox->on_change = [this](int value) {
        s_settings.page_size = value;
        rebuild_preview();
        set_modified(true);
    };
    auto& spin_note = spin_row.add<GUI::Label>("默认 9 个；此处修改会同步写入 rime 的 menu/page_size。"_string);
    spin_note.set_text_alignment(Gfx::TextAlignment::CenterLeft);

    // Reuse the real candidate-window widget so the preview is pixel-identical
    // to what LibGUI apps (and the GTK immodule's Serenity-styled popup) show.
    auto& preview_frame = candidates.add<GUI::Widget>();
    preview_frame.set_layout<GUI::HorizontalBoxLayout>(0);
    preview_frame.set_fixed_height(40);
    preview_frame.set_fill_with_background_color(true);
    m_preview = preview_frame.add<GUI::InputMethodPanelWidget>();
    m_preview->set_fixed_height(32);
    // A stretching spacer keeps the content-sized panel left-aligned; without
    // it the horizontal box would centre the panel in the group.
    auto& preview_spacer = preview_frame.add<GUI::Widget>();
    preview_spacer.set_fill_with_background_color(true);

    m_status_label = add<GUI::Label>("就绪 · 修改后点「应用」写入并部署 rime 配置"_string);
    m_status_label->set_text_alignment(Gfx::TextAlignment::CenterLeft);
    m_status_label->set_fixed_height(22);

    refresh_controls();
    return {};
}

void IMESettingsTab::rebuild_preview()
{
    Vector<String> samples {
        "你好"_string, "妳好"_string, "利好"_string, "立好"_string, "理好"_string,
        "立號"_string, "里豪"_string, "逆號"_string, "擬好"_string, "擬號"_string,
    };
    Vector<String> labels;
    Vector<String> texts;
    int count = min(s_settings.page_size, static_cast<int>(samples.size()));
    for (int i = 0; i < count; ++i) {
        labels.append(String::formatted("{} ", i + 1).release_value_but_fixme_should_propagate_errors());
        texts.append(samples[i]);
    }
    m_preview->set_content(String {}, 0, move(labels), move(texts), 0, String {}, false, true);

    // The panel paints its own cold-grey background across the whole widget.
    // Left stretched by the vertical box layout it leaves a long empty strip
    // after the ">" pager hint; pin it to the width of its actual content.
    auto preferred = m_preview->preferred_size();
    m_preview->set_fixed_width(max(preferred.width(), 48));
}

void IMESettingsTab::refresh_controls()
{
    m_enabled_checkbox->set_checked(s_settings.enabled, GUI::AllowCallback::No);
    for (size_t i = 0; i < s_fuzzy_option_count && i < m_fuzzy_checkboxes.size(); ++i)
        m_fuzzy_checkboxes[i]->set_checked(s_settings.*(s_fuzzy_options[i].flag), GUI::AllowCallback::No);
    m_page_size_spinbox->set_value(s_settings.page_size, GUI::AllowCallback::No);
    rebuild_preview();
}

void IMESettingsTab::apply_settings()
{
    s_last_result = apply_to_disk();
    m_status_label->set_text(as_string(s_last_result));
}

void IMESettingsTab::cancel_settings()
{
    load_from_disk();
    refresh_controls();
    m_status_label->set_text("已放弃修改，恢复到磁盘上的当前配置"_string);
}

void IMESettingsTab::reset_default_values()
{
    reset_to_defaults();
    refresh_controls();
    m_status_label->set_text("已恢复默认值：5 组模糊音 + 简拼/纠错 · 候选 9 个 · 仅 Rime（点应用生效）"_string);
}

} // namespace

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    auto window = TRY(GUI::SettingsWindow::create("输入法设置", GUI::SettingsWindow::ShowDefaultsButton::Yes));
    (void)TRY(window->add_tab<IMESettingsTab>("输入法与候选"_string, "ime"sv));
    window->resize(760, 600);
    window->show();
    return app->exec();
}
