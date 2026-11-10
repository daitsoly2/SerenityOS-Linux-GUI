/*
 * serenity-ime-settings —— Serenity 输入法的控制面板小程序。
 *
 * 后端是 serenity-engine（直链 librime），不再经过 fcitx5 / serenityime addon。
 * 这里直接写 rime 的 custom YAML（移植版没有引 rime 配置库），然后通过控制
 * socket 让引擎重新部署；开关状态同样通过控制 socket 查询与切换。
 *
 * 产品约束：
 *   1. 中文输入法只有 Rime，另一档是纯英文键盘；没有 UI 能加第三种引擎。
 *   2. 只提供常见模糊音，f/h 与 g/k 两组明确不提供、也不会写进 YAML。
 *   3. 候选预览默认 9 个、字号默认 14 磅，用的是真实候选窗同一个
 *      GUI::InputMethodPanelWidget；字号写进用户配置，候选面板每次刷新重读，
 *      不重启应用即可生效。
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    int candidate_font_size { GUI::default_candidate_font_size };
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
    /* 必须和 serenity-engine 的 default_user_data_dir() 用同一套优先级，
     * 否则设置写到一个目录、引擎读另一个目录，改了等于没改。
     * 保留 SERENITY_IME_USER_DIR 作为旧配置的兼容别名。 */
    if (char const* value = getenv("RIME_USER_DATA_DIR"); value && *value)
        return value;
    if (char const* value = getenv("SERENITY_IME_USER_DIR"); value && *value)
        return value;
    return ByteString::formatted("{}/.local/share/rime", home_directory().characters());
}

ByteString luna_custom_path()
{
    return ByteString::formatted("{}/luna_pinyin.custom.yaml", rime_user_directory().characters());
}

ByteString default_custom_path()
{
    return ByteString::formatted("{}/default.custom.yaml", rime_user_directory().characters());
}

ByteString status_directory()
{
    char const* dir = getenv("SERENITY_IME_STATUS_DIR");
    return (dir && *dir) ? ByteString { dir } : ByteString { "/tmp" };
}

ByteString control_socket_path()
{
    return ByteString::formatted("{}/serenity-ime-ctl.sock", status_directory().characters());
}

ByteString status_file_path()
{
    return ByteString::formatted("{}/serenity-ime-status", status_directory().characters());
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

// 向控制 socket 发一条命令并读回一行应答；引擎没起来时返回空串。
ByteString query_control(StringView command)
{
    auto path = control_socket_path();
    if (path.length() >= sizeof(sockaddr_un::sun_path))
        return {};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return {};
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.characters(), path.length());
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return {};
    }
    auto line = ByteString::formatted("{}\n", command);
    (void)send(fd, line.characters(), line.length(), MSG_NOSIGNAL);
    char buffer[256];
    auto count = recv(fd, buffer, sizeof(buffer) - 1, 0);
    close(fd);
    if (count <= 0)
        return {};
    buffer[count] = '\0';
    return ByteString { buffer };
}

// 开关状态归引擎管：先问控制 socket，问不到再读它发布的状态文件。
// 旧引擎的 status 应答没有 active 字段，也走这条回退路径。
bool engine_is_active()
{
    auto reply = query_control("status"sv);
    if (char const* active = strstr(reply.characters(), "active "))
        return active[7] == '1';
    auto status = read_whole_file(status_file_path());
    return status.is_empty() || status.characters()[0] != '0';
}

// 生成的 YAML 是唯一事实来源：读回来显示，保证界面和 rime 实际用到的一致，
// 哪怕有人手工改过文件。
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

    s_settings.enabled = engine_is_active();
    s_settings.candidate_font_size = GUI::candidate_font_size();
}

void reset_to_defaults()
{
    s_settings = IMESettings {};
}

// ------------------------------------------------------- external actions --

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

    // 候选字号写进用户配置，候选面板每次刷新都会重读，所以不用重启引擎，
    // 下一次打字就生效。同时清掉本进程的预览覆盖，让预览回到真实配置值。
    bool wrote_font_size = GUI::set_candidate_font_size(s_settings.candidate_font_size);
    GUI::set_candidate_font_size_preview_override(-1);

    // 引擎可达时，先让它把开关状态写进 status 文件，再发 reload 重新部署。
    // 顺序不能反：引擎重启后 initial_active() 会读 status 文件恢复开关，
    // 这样「关闭输入法」不会被重启偷偷改回中文。
    bool engine_reachable = !query_control("status"sv).is_empty();
    if (engine_reachable) {
        send_control_command(s_settings.enabled ? "set rime"sv : "toggle off"sv);
        // 引擎收到 reload 会优雅退出，serenitid 立刻拉回新进程；
        // 启动时的 start_maintenance(True) 会重新编译 custom YAML，
        // 模糊音与候选数这时才真正生效。
        send_control_command("reload"sv);
    }

    return ByteString::formatted(
        "已写入 · 模糊音 {}/{} 组 · 候选 {} 个 · 字号 {} · {} · {}",
        enabled_fuzzy, s_fuzzy_option_count, s_settings.page_size,
        s_settings.candidate_font_size,
        engine_reachable ? "引擎正在重新部署（约 1 秒后生效）" : "引擎未运行，下次启动输入法时生效",
        (!wrote_yaml || !wrote_default || !wrote_font_size) ? "部分文件不可写" : "仅 Rime");
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
    RefPtr<GUI::SpinBox> m_font_size_spinbox;
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

    // 候选字体大小与 rime 无关：写进用户配置，候选面板每次刷新重读。
    auto& font_row = candidates.add<GUI::Widget>();
    font_row.set_layout<GUI::HorizontalBoxLayout>(6);
    font_row.set_fixed_height(26);
    auto& font_label = font_row.add<GUI::Label>("候选字体大小："_string);
    font_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    font_label.set_fixed_width(110);
    m_font_size_spinbox = font_row.add<GUI::SpinBox>();
    m_font_size_spinbox->set_fixed_width(80);
    m_font_size_spinbox->set_range(GUI::min_candidate_font_size, GUI::max_candidate_font_size, GUI::AllowCallback::No);
    m_font_size_spinbox->on_change = [this](int value) {
        s_settings.candidate_font_size = value;
        // 预览即时跟随；真实候选窗在点「应用」写入配置后才变。
        GUI::set_candidate_font_size_preview_override(value);
        rebuild_preview();
        set_modified(true);
    };
    auto& font_note = font_row.add<GUI::Label>("默认 14 磅；点「应用」后下一次打字即生效。"_string);
    font_note.set_text_alignment(Gfx::TextAlignment::CenterLeft);

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

    m_status_label = add<GUI::Label>("就绪 · 修改后点「应用」写入配置并重新部署 rime"_string);
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

    // 预览条按内容宽度固定，横向排布的候选项才不会被拉伸出空白尾巴；
    // 但字号调大后内容宽度会超过窗口，把底部「取消 / 应用」挤出可视区，
    // 所以用父容器宽度封顶（布局还没算好时回退到窗口内容宽度）。
    int max_width = 700;
    if (auto* parent = m_preview->parent_widget(); parent && parent->width() > 48)
        max_width = parent->width() - 12;
    auto preferred = m_preview->preferred_size();
    int width = max(preferred.width(), 48);
    if (width > max_width)
        width = max_width;
    m_preview->set_fixed_width(width);
}

void IMESettingsTab::refresh_controls()
{
    m_enabled_checkbox->set_checked(s_settings.enabled, GUI::AllowCallback::No);
    for (size_t i = 0; i < s_fuzzy_option_count && i < m_fuzzy_checkboxes.size(); ++i)
        m_fuzzy_checkboxes[i]->set_checked(s_settings.*(s_fuzzy_options[i].flag), GUI::AllowCallback::No);
    m_page_size_spinbox->set_value(s_settings.page_size, GUI::AllowCallback::No);
    m_font_size_spinbox->set_value(s_settings.candidate_font_size, GUI::AllowCallback::No);
    // 「取消」会走到这里：丢掉未应用的预览覆盖，回到磁盘上的真实配置。
    GUI::set_candidate_font_size_preview_override(-1);
    rebuild_preview();
}

void IMESettingsTab::apply_settings()
{
    s_last_result = apply_to_disk();
    // apply_to_disk() 已清掉预览覆盖，让预览与刚写入的配置保持一致。
    rebuild_preview();
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
    // 预览按默认值显示，但磁盘配置未变；点「应用」才写入。
    GUI::set_candidate_font_size_preview_override(GUI::default_candidate_font_size);
    rebuild_preview();
    m_status_label->set_text("已恢复默认值：常见模糊音 + 简拼/纠错 · 候选 9 个 · 字号 14 · 仅 Rime（点应用生效）"_string);
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
