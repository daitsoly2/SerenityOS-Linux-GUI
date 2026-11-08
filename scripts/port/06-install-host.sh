#!/usr/bin/env bash
# Put the SerenityOS desktop on this machine as the thing that boots.
#
#   sudo scripts/port/06-install-host.sh [--no-audio] [--keep-display-manager]
#
# Everything lands under one prefix (/serenity): binaries, libs, resources,
# config. /res and /etc/{WindowServer.ini,serenity} stay behind as symlinks into
# it, because Serenity code hardcodes those paths.
#
# What this does, in order:
#   1. session user "serenity" (unprivileged) + groups it needs for the devices
#   2. prefix install through 05-install-prefix.sh --systemd
#   3. /etc/serenity/session.conf: Autologin + language
#   4. serenity_compat.ko so AudioServer finds /dev/audio/0   (--no-audio skips)
#   5. boot switch: stop+disable the display manager, enable serenity-desktop
#
# Nothing here starts the desktop: reboot when you are ready, or run
#   sudo systemctl start serenity-desktop.service
# Rollback (also installed as <prefix>/bin/serenity-restore-gnome):
#   sudo systemctl disable --now serenity-desktop.service
#   sudo systemctl enable --now gdm3
#   sudo systemctl set-default graphical.target
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
PREFIX="${PREFIX:-/serenity}"
SESSION_USER="${SERENITY_USER:-serenity}"
SESSION_LANGUAGE="${SERENITY_LANGUAGE:-zh-CN}"

WITH_AUDIO=1
# fcitx5 已废弃：新系统的输入法由 /serenity/bin/serenity-engine 直连 librime
# 提供（SessionManager spawn 的就是它），不再需要 fcitx5、fcitx5-rime、
# libfcitx5core-dev 或 serenityime addon。默认关掉；下面的旧块只留作历史参考，
# 不要再启用 -- 它装回来的 fcitx5 会和 serenity-engine 抢 XIM/socket。
WITH_IME=0
SWITCH_SESSION=1
# 会话用户的密码：桌面里按下 sudo / 需要认证的 polkit 操作时要用。没有密码的话
# 在 Serenity 桌面里什么事都干不了（而且 LoginWindow 也进不去）。
SESSION_PASSWORD="${SERENITY_PASSWORD:-serenity}"
for arg in "$@"; do
    case "${arg}" in
        --no-audio) WITH_AUDIO=0 ;;
        --no-ime) WITH_IME=0 ;;
        --keep-display-manager) SWITCH_SESSION=0 ;;
        *) echo "unknown option: ${arg}" >&2; exit 2 ;;
    esac
done

if [[ "${EUID}" -ne 0 ]]; then
    echo "需要 root：sudo $0 $*" >&2
    exit 1
fi

for binary in serenity-window-server serenity-shell serenitid; do
    if [[ ! -x "${BUILD}/bin/${binary}" ]]; then
        echo "没有构建产物 ${BUILD}/bin/${binary}" >&2
        echo "先构建：cmake --build ${BUILD} --target serenity-window-server serenity-shell serenitid" >&2
        exit 1
    fi
done

# 本脚本只「安装」不「构建」：把过期的产物装上去，最后表现就是「改了代码但桌面
# 行为没变」，极难查（真机调试里踩过）。让 ninja 自己回答还有没有欠账。
if command -v ninja >/dev/null 2>&1 && [[ -f "${BUILD}/build.ninja" ]]; then
    pending="$(cd "${BUILD}" && ninja -n 2>/dev/null | grep -c '^\[' || true)"
    if (( pending > 0 )); then
        echo "构建产物不是最新的（ninja 还有 ${pending} 步没做），先构建再安装：" >&2
        echo "  cmake --build ${BUILD} -j\$(nproc)" >&2
        exit 1
    fi
fi

# --- 1. 会话用户 ----------------------------------------------------------
if ! id -u "${SESSION_USER}" >/dev/null 2>&1; then
    echo "== 创建会话用户 ${SESSION_USER} =="
    useradd --create-home --shell /bin/bash "${SESSION_USER}"
else
    echo "== 会话用户 ${SESSION_USER} 已存在 =="
fi

# DRM(video/render)、evdev+libinput(input)、/dev/audio(audio)、Escalator 查
# /etc/shadow(shadow)。缺哪个组就跳过，装完可再补。
# sudo 组：桌面里开个终端装包/改系统要用，没有它就真的「sudo 都没法」。
groups_to_add=()
for group in video render input audio shadow plugdev netdev sudo; do
    if getent group "${group}" >/dev/null 2>&1; then
        groups_to_add+=("${group}")
    fi
done
if (( ${#groups_to_add[@]} )); then
    usermod -aG "$(IFS=,; echo "${groups_to_add[*]}")" "${SESSION_USER}"
fi

# 密码：只有账号处于「锁定/无密码」状态时才设置，不覆盖用户后来改过的密码。
if passwd -S "${SESSION_USER}" 2>/dev/null | awk '{print $2}' | grep -qE '^(L|NP)$'; then
    echo "${SESSION_USER}:${SESSION_PASSWORD}" | chpasswd
    echo "== 已设置 ${SESSION_USER} 的密码：${SESSION_PASSWORD}（可用 SERENITY_PASSWORD 覆盖）=="
else
    echo "== ${SESSION_USER} 已有密码，保持不变（忘了就用 sudo passwd ${SESSION_USER} 重置）=="
fi

SESSION_UID="$(id -u "${SESSION_USER}")"
SESSION_HOME="$(getent passwd "${SESSION_USER}" | cut -d: -f6)"
echo "   uid=${SESSION_UID} home=${SESSION_HOME} groups=$(id -Gn "${SESSION_USER}")"

# --- 2. 前缀安装 ----------------------------------------------------------
SERENITY_USER="${SESSION_USER}" PREFIX="${PREFIX}" \
    "${ROOT}/scripts/port/05-install-prefix.sh" --systemd

# --- 3. 会话配置 ----------------------------------------------------------
# Serenity 写死读 /etc/serenity/session.conf；真实文件在 prefix 里，外面留软链。
install -d "${PREFIX}/etc/serenity"
if [[ ! -f "${PREFIX}/etc/serenity/session.conf" ]]; then
    cat > "${PREFIX}/etc/serenity/session.conf" <<EOF
[Session]
# 开机直接进桌面：不设这里就会停在 LoginWindow 等密码（serenity 用户没有密码）。
Autologin=${SESSION_USER}
Language=${SESSION_LANGUAGE}
Restart=yes
EOF
fi
[[ -e /etc/serenity ]] || ln -s "${PREFIX}/etc/serenity" /etc/serenity

# WindowServer 的主题/布局配置：没有它就用 Base/etc 的默认值起一份。
if [[ ! -f "${PREFIX}/etc/WindowServer.ini" ]]; then
    install -m 644 "${ROOT}/Base/etc/WindowServer.ini" "${PREFIX}/etc/WindowServer.ini"
fi
[[ -e /etc/WindowServer.ini ]] || ln -s "${PREFIX}/etc/WindowServer.ini" /etc/WindowServer.ini

# 配置目录交给会话用户：否则主题换完写不回去（WindowServer 只能打警告）。
chown -R "${SESSION_USER}:$(id -gn "${SESSION_USER}")" "${PREFIX}/etc"

# --- 4. 中文字体 + 输入法 -------------------------------------------------
# Serenity 的 FontDatabase 只从 <prefix>/res/fonts 递归加载 .ttf/.font（没有走
# fontconfig），所以系统里装了中文字体也不会被桌面用上——缺字只会画成方框。
# 必须把字体文件本身放进 res/fonts，字形缺失时才能回退到它。
CJK_FONT="$(find /usr/share/fonts -iname 'SarasaGothicSC*Regular*.ttf' -print -quit 2>/dev/null)"
[[ -n "${CJK_FONT}" ]] || CJK_FONT="$(find /usr/share/fonts -iname 'NotoSansCJKsc*Regular*.ttf' -print -quit 2>/dev/null)"
[[ -n "${CJK_FONT}" ]] || CJK_FONT="$(find /usr/share/fonts -iname 'wqy-microhei.ttc' -print -quit 2>/dev/null)"
if [[ -n "${CJK_FONT}" ]]; then
    install -d "${PREFIX}/res/fonts"
    font_dir="$(dirname "${CJK_FONT}")"
    # 只装常规体 + 粗体：Serenity 会把 res/fonts 里的每个 .ttf 都加载进来，Sarasa
    # 每个字重约 23MB，全拷进去就是几百 MB 的内存和启动时间。
    install -m 644 "${CJK_FONT}" "${PREFIX}/res/fonts/$(basename "${CJK_FONT}")"
    bold_font="${CJK_FONT/Regular/Bold}"
    [[ -f "${bold_font}" ]] && install -m 644 "${bold_font}" "${PREFIX}/res/fonts/$(basename "${bold_font}")"
    echo "== 中文字体已装进 ${PREFIX}/res/fonts：$(basename "${CJK_FONT}")$( [[ -f "${bold_font}" ]] && echo ' + 粗体' ) =="
else
    echo "!! 没找到中文字体（Sarasa/Noto CJK/文泉驿），桌面会显示方框" >&2
    echo "!! 装一个即可：sudo apt install fonts-noto-cjk" >&2
fi

# [RETIRED] 以下 fcitx5 安装/配置/addon 逻辑已停用，保留仅供回溯，勿再启用。
if [[ "${WITH_IME}" == "1" ]]; then
    if ! command -v fcitx5 >/dev/null 2>&1; then
        echo "== 安装 fcitx5（中文输入法）=="
        DEBIAN_FRONTEND=noninteractive apt-get update -qq || true
        DEBIAN_FRONTEND=noninteractive apt-get install -y fcitx5 fcitx5-chinese-addons fcitx5-rime \
            || echo "!! fcitx5 安装失败，先跳过（桌面里打字只有英文）" >&2
    fi
    # fcitx5 由 serenitd 作为会话子进程拉起（需要 D-Bus 会话，serenitid 会先起
    # dbus-daemon）。这里只保证 GTK/Qt 客户端认识 fcitx（环境变量在 serenitid 里设）。
    install -d "${SESSION_HOME}/.config/fcitx5"
    if [[ ! -f "${SESSION_HOME}/.config/fcitx5/profile" ]] && command -v fcitx5 >/dev/null 2>&1; then
        cat > "${SESSION_HOME}/.config/fcitx5/profile" <<'FCITX'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=pinyin

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=pinyin
Layout=

[GroupOrder]
0=Default
FCITX
        chown -R "${SESSION_USER}:$(id -gn "${SESSION_USER}")" "${SESSION_HOME}/.config"
    fi
    echo "== 输入法：$(command -v fcitx5 || echo '未安装')，由 serenitid 随会话启动 =="

    # Serenity 原生应用（LibGUI）不走 GTK/Qt 的 IM 模块，它连的是 fcitx5 的
    # serenityime addon（unix socket）。addon 要 fcitx5 的核心头文件才能编，
    # 所以只能在 fcitx5 装好之后再构建一次。
    if command -v fcitx5 >/dev/null 2>&1; then
        [[ -d /usr/include/Fcitx5Core ]] \
            || DEBIAN_FRONTEND=noninteractive apt-get install -y libfcitx5core-dev >/dev/null 2>&1 || true
        if [[ -d /usr/include/Fcitx5Core ]]; then
            builder="${SUDO_USER:-}"
            if [[ -n "${builder}" && "${builder}" != "root" ]]; then
                echo "== 以 ${builder} 构建 serenityime addon =="
                if runuser -u "${builder}" -- cmake -S "${ROOT}/Ports/LinuxGUI/IMEFrontend" \
                        -B "${ROOT}/Build/ime" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null \
                    && runuser -u "${builder}" -- cmake --build "${ROOT}/Build/ime" -j"$(nproc)"; then
                    addon_dir="$(find /usr/lib -maxdepth 4 -type d -name fcitx5 2>/dev/null | head -n 1)"
                    if [[ -n "${addon_dir}" ]]; then
                        install -m755 "${ROOT}/Build/ime/bin/libserenityime.so" "${addon_dir}/"
                        install -d /usr/share/fcitx5/addon
                        install -m644 "${ROOT}/Ports/LinuxGUI/IMEFrontend/serenityime.conf" /usr/share/fcitx5/addon/
                        echo "== serenityime addon 已装到 ${addon_dir}（LibGUI 应用的中文输入通道）=="
                    fi
                else
                    echo "!! serenityime addon 构建失败，Serenity 应用只能打英文（GTK 应用不受影响）" >&2
                fi
            fi
        else
            echo "!! 没有 libfcitx5core-dev，跳过 serenityime addon（Serenity 应用无法输中文）" >&2
        fi
    fi
fi

# --- 5. 音频兼容模块（可选）---------------------------------------------
if [[ "${WITH_AUDIO}" == "1" ]]; then
    if [[ -d "/lib/modules/$(uname -r)/build" ]]; then
        echo "== 构建 serenity_compat.ko（$(uname -r)）=="
        make -C "${ROOT}/KernelModules/serenity_compat" install
        # make 以 root 跑，产物留在源码树里；还回去免得以后用户构建写不进去。
        if [[ -n "${SUDO_UID:-}" ]]; then
            chown -R "${SUDO_UID}:${SUDO_GID:-${SUDO_UID}}" "${ROOT}/KernelModules/serenity_compat"
        fi
    else
        echo "!! 没有 /lib/modules/$(uname -r)/build，跳过音频模块（桌面照常，只是没声音）" >&2
    fi
fi

# --- 5. 切换开机进入的会话 ------------------------------------------------
cat > "${PREFIX}/bin/serenity-restore-gnome" <<'EOF'
#!/bin/sh
# 从 Serenity 桌面回到 GNOME。
set -e
systemctl disable --now serenity-desktop.service
systemctl enable --now gdm3
systemctl set-default graphical.target
echo "已恢复 GNOME（gdm3）。重启或 ctrl-alt-F2 后登录。"
EOF
chmod 755 "${PREFIX}/bin/serenity-restore-gnome"

if [[ "${SWITCH_SESSION}" == "1" ]]; then
    # 只 disable 不 stop：gdm3 活着的时候是 DRM master，WindowServer 抢不到屏幕，
    # 但停掉它会连当前 GNOME 会话一起端掉。下次开机它就不启动了，需要手动切
    # 会话时再停（见下面的输出）。
    if systemctl cat gdm3.service >/dev/null 2>&1; then
        echo "== 停用 gdm3 开机自启，改由 serenity-desktop.service 接管屏幕 =="
        systemctl disable gdm3
    fi
    systemctl enable serenity-desktop.service
    # multi-user：graphical.target 只是"再拉一个 display-manager"，这里没有
    # display manager 了，写清楚免得误导。
    systemctl set-default multi-user.target
fi

cat <<EOF

== 完成 ==
  prefix      ${PREFIX}
  会话用户    ${SESSION_USER} (uid ${SESSION_UID})  密码 ${SESSION_PASSWORD}
  会话单元    serenity-desktop.service $([[ "${SWITCH_SESSION}" == "1" ]] && echo "已 enable，gdm3 已停用" || echo "未启用")
  日志        /tmp/serenity-session.log（SERENITY_DEBUG=1 可看控制台输出）

现在启动桌面 —— **直接重启**（最快的路径）：
  sudo reboot

不重启、当场切：你的终端就跑在 GNOME 里，\`systemctl stop gdm3\` 会连终端和后面
的命令一起杀掉，所以整串必须交给 PID 1：
  sudo systemd-run --unit=serenity-switch --collect /bin/sh -c \\
      'sleep 1; systemctl stop gdm3; sleep 2; systemctl start serenity-desktop'

回到 GNOME（黑屏时先 Ctrl+Alt+F3 登录 ${SUDO_USER:-root}）：
  sudo ${PREFIX}/bin/serenity-restore-gnome

桌面里要 root：终端里 \`sudo\` 用上面那个密码（${SESSION_USER} 已在 sudo 组）。

在图形终端里跑一个应用（**必须**在桌面会话已启动之后；没有 WindowServer 时
应用只会报 "cannot connect to the WindowServer" 然后退出）：
  sudo -u ${SESSION_USER} env SERENITY_PREFIX=${PREFIX} SERENITY_BIN=${PREFIX}/bin \\
      ${PREFIX}/bin/serenity-terminal
EOF
