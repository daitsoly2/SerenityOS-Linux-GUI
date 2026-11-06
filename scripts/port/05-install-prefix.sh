#!/usr/bin/env bash
# Install the desktop runtime into one prefix instead of scattering it over the
# host's /usr/local.
#
#   sudo scripts/port/05-install-prefix.sh [--systemd] [--clean]
#
# Layout (PREFIX=/serenity by default):
#
#   <prefix>/bin              serenity-* + the serenitid wrapper
#   <prefix>/lib              liblagom-*.so* (RUNPATH is $ORIGIN/../lib)
#   <prefix>/libexec          serenitid.real
#   <prefix>/res              fonts, icons, themes, locale, apps (Root/res)
#   <prefix>/share/desktop    system-provided desktop shortcuts
#   <prefix>/etc              WindowServer.ini, serenity/session.conf
#
# Serenity code hardcodes /res, /etc/WindowServer.ini and /etc/serenity, so those
# stay behind as symlinks into the prefix. The desktop shell itself looks for
# <prefix>/share/desktop (falling back to /usr/share/serenity/desktop).
#
#   --systemd   (re)write /etc/systemd/system/serenity-desktop.service to run
#               <prefix>/bin/serenitid
#   --presenter=kms|wlroots
#               pick the WindowServer presenter (default: wlroots). wlroots runs
#               LIBINPUT + KMS inside WindowServer (needs seatd and the session
#               user in the `video` group); kms is the legacy dumb-buffer path.
#   --clean     move the old /usr/local/bin/serenity-* and
#               /usr/local/lib/liblagom-* into <prefix>/.old-usr-local-<date>/
#               instead of leaving two copies around (reversible).
set -euo pipefail

# 现场留档：这套脚本中途失败时（Text file busy、cp 失败……）终端常常已经滚过去了，
# 只能靠「装了一半」反推。输出同时进 /tmp/serenity-05.log，失败行号也记进去。
INSTALL_LOG="${INSTALL_LOG:-/tmp/serenity-05.log}"
exec > >(tee -a "${INSTALL_LOG}") 2>&1
echo "== $(date '+%F %T') 05-install-prefix 开始（log: ${INSTALL_LOG}）=="
trap 'printf "[05] 终止于第 %s 行（退出码 %s）\n" "${LINENO}" "$?"' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
PREFIX="${PREFIX:-/serenity}"

# 会话用户：--systemd 写 unit 时要它的 uid/home，不能写死 1000 —— 本机 uid 1000
# 往往已经是安装者自己，serenity 只能拿到别的号。
SESSION_USER="${SERENITY_USER:-serenity}"
SESSION_UID="$(id -u "${SESSION_USER}" 2>/dev/null || echo 1000)"
SESSION_HOME="$(getent passwd "${SESSION_USER}" 2>/dev/null | cut -d: -f6)"
[[ -n "${SESSION_HOME}" ]] || SESSION_HOME="/home/${SESSION_USER}"
SESSION_GROUP="$(id -gn "${SESSION_USER}" 2>/dev/null || echo "${SESSION_UID}")"

# 默认就写 systemd unit：在真机上装完却忘了加 --systemd，结果是「二进制和库都是新
# 的、unit 还是旧的」，排查时极难看出来（脚本还照样打印「完成」）。要跳过用
# --no-systemd。
INSTALL_SYSTEMD=1
CLEAN_OLD=0
PRESENTER="${SERENITY_PRESENTER_DEFAULT:-wlroots}"
for arg in "$@"; do
    case "${arg}" in
        --systemd) INSTALL_SYSTEMD=1 ;;
        --no-systemd) INSTALL_SYSTEMD=0 ;;
        --presenter=*) PRESENTER="${arg#--presenter=}" ;;
        --clean) CLEAN_OLD=1 ;;
        *) echo "unknown option: ${arg}" >&2; exit 2 ;;
    esac
done

if [[ ! -x "${BUILD}/bin/serenity-window-server" ]]; then
    echo "没有找到构建产物：${BUILD}/bin/serenity-window-server" >&2
    echo "先在目标机上构建：cmake --build ${BUILD} --target serenity-window-server serenity-shell serenitid" >&2
    exit 1
fi

echo "== 安装到 ${PREFIX} =="
install -d "${PREFIX}"/{bin,lib,libexec,res,share/desktop,share/templates,etc/serenity}

echo "== [1/6] 安装二进制 =="

# --- 二进制 ---------------------------------------------------------------
# 全部「先装到临时名再 mv」：直接覆盖正在运行的二进制会 ETXTBSY（Text file busy）
# 让整个脚本 set -e 中断，装了一半（二进制换了、unit 还是旧的）。rename 换的是
# inode，正在跑的进程照旧用旧 inode，更新因此不需要先停会话。
install_atomic()
{
    local source="$1" destination="$2" mode="${3:-755}"
    local temporary="${destination%/*}/.$(basename "${destination}").new"
    install -m "${mode}" "${source}" "${temporary}"
    mv -f "${temporary}" "${destination}"
}

# serenitid 例外：真二进制放 libexec，bin 里留包装脚本（同名互覆盖会死循环）。
for binary in "${BUILD}"/bin/serenity-*; do
    install_atomic "${binary}" "${PREFIX}/bin/$(basename "${binary}")"
done
install_atomic "${BUILD}/bin/serenitid" "${PREFIX}/libexec/serenitid.real"

# Task manager: Win7's "Show processes from all users" / "End Process" need to
# read and signal processes owned by other users. Give the ELF four narrow file
# capabilities instead of making a whole GUI process setuid-root.
# setcap lives in /usr/sbin on Debian, which may not be on the caller's PATH.
if [ -x "${PREFIX}/bin/serenity-system-monitor" ]; then
    SETCAP_BIN="$(command -v setcap || true)"
    [ -n "${SETCAP_BIN}" ] || SETCAP_BIN=/usr/sbin/setcap
    if [ -x "${SETCAP_BIN}" ]; then
        "${SETCAP_BIN}" 'cap_dac_read_search,cap_kill,cap_sys_ptrace,cap_sys_nice+ep' \
            "${PREFIX}/bin/serenity-system-monitor" \
            || echo "!! setcap failed: task manager will only manage this user's processes" >&2
    else
        echo "!! libcap2-bin missing: install it to grant task manager cross-user access" >&2
    fi
fi

cat > "${PREFIX}/bin/serenitid" <<EOF
#!/bin/sh
# Session entry point: one prefix, one environment.
export SERENITY_PREFIX="${PREFIX}"
export SERENITY_BIN="${PREFIX}/bin"
export PATH="${PREFIX}/bin:\$PATH"
exec "${PREFIX}/libexec/serenitid.real" "\$@"
EOF
chmod 755 "${PREFIX}/bin/serenitid"

# Windows 应用的统一入口现在是原生二进制 serenity-winapp（由上面的 serenity-* 循环
# 一并安装），启动策略（app/game、运行时、env、前缀准备）内置于 C++，对齐 omikuji。
# 旧的 sh 脚本 winapp-launcher.sh 仅作历史参考，不再安装。
# 中文字体：把 CJK 字体装进 Wine 前缀并注册 FontSubstitutes，避免中文应用显示成方框。
# 由 serenity-winapp 首次启动时自动调用，也可以手动跑：
#   sudo <prefix>/bin/serenity-wine-fonts --prefix /root/.wine-dw
install -m755 "${ROOT}/scripts/port/wine-fonts.py" "${PREFIX}/bin/serenity-wine-fonts"
# 浏览器：用宿主 Firefox 当引擎（X11 桥映射成 Serenity 窗口）。
# 名字必须和 hostify 的映射一致：/bin/WebBrowser -> serenity-web-browser。
install -m755 "${ROOT}/scripts/port/webbrowser-launcher.sh" "${PREFIX}/bin/serenity-web-browser"
# Chromium：Debian Chromium 内核，通过 Wayland 直接映射成 Serenity 窗口。
# 名字必须和 hostify 的映射一致：/bin/Chromium -> serenity-chromium。
install -m755 "${ROOT}/scripts/port/chromium-launcher.sh" "${PREFIX}/bin/serenity-chromium"
# 文本编辑器：TextEditor 没移植，换成宿主 Linux 编辑器（名字映射保持一致）。
install -m755 "${ROOT}/scripts/port/texteditor-launcher.sh" "${PREFIX}/bin/serenity-text-editor"
# 退出对话框（ShutdownDialog）spawn 的是 /bin/{logout,shutdown,reboot}；hostify
# 会把它们翻成下列名字。同一个脚本装三份，按 $0 分派。
for name in logout shutdown reboot; do
    install -m755 "${ROOT}/scripts/port/session-control.sh" "${PREFIX}/bin/serenity-${name}"
done

echo "== [2/6] 安装库 =="

# --- 库 -------------------------------------------------------------------
# 二进制的 RUNPATH 是 $ORIGIN/../lib，所以放在 <prefix>/lib 就够；ldconfig 只是
# 让系统里其它消费者（如果有）也能找到。
# 同样走临时名 + rename：原地覆盖一个已经被进程 mmap 的 .so 会让桌面当场崩掉。
for library in "${BUILD}"/lib/liblagom-*.so*; do
    name=$(basename "${library}")
    case "${name}" in
        *.so|*.so.*) ;;
        *) continue ;;
    esac
    # 只装实体文件；.so / .so.0 这些是构建目录里的符号链接，装成软链会指回 Build。
    if [[ -L "${library}" ]]; then
        ln -sfn "$(basename "$(readlink "${library}")")" "${PREFIX}/lib/.${name}.new"
        mv -f "${PREFIX}/lib/.${name}.new" "${PREFIX}/lib/${name}"
    else
        install_atomic "${library}" "${PREFIX}/lib/${name}" 644
    fi
done
ldconfig

echo "== [3/6] 安装资源 =="

# --- 资源 -----------------------------------------------------------------
# Root/res 里大部分目录是指向 Base/res 的符号链接（见 03-run.sh）。用 -L 复制成
# 实体文件，否则 <prefix>/res 又会指回源码树，"分离"就白做了。
# 先装到临时目录再整体替换，避免往已有符号链接里写东西。
staging="${PREFIX}/.res.new"
rm -rf "${staging}"
mkdir -p "${staging}"
cp -aL "${BUILD}/Root/res/." "${staging}/"
# locale 是真实拷贝、可能比源码旧，用 Base/res 的最新文案覆盖。
cp -a "${ROOT}/Base/res/locale/." "${staging}/locale/"
rm -rf "${PREFIX}/res.old"
if [[ -d "${PREFIX}/res" ]]; then
    mv "${PREFIX}/res" "${PREFIX}/res.old"
fi
mv "${staging}" "${PREFIX}/res"
rm -rf "${PREFIX}/res.old"

# --- 系统默认快捷方式 -----------------------------------------------------
if [[ -d "${ROOT}/Base/res/desktop" ]]; then
    cp -a "${ROOT}/Base/res/desktop/." "${PREFIX}/share/desktop/"
elif [[ -d /usr/share/serenity/desktop && ! -L /usr/share/serenity/desktop ]]; then
    # 从旧布局迁移
    cp -a /usr/share/serenity/desktop/. "${PREFIX}/share/desktop/"
fi

# --- 桌面右键「新建」用的文件模板 ----------------------------------------
if [[ -d "${ROOT}/Base/res/templates" ]]; then
    install -d "${PREFIX}/share/templates"
    cp -a "${ROOT}/Base/res/templates/." "${PREFIX}/share/templates/"
fi

echo "== [4/6] 配置与软链 =="

# --- 配置：文件进 prefix，原位置留符号链接 --------------------------------
if [[ -f /etc/WindowServer.ini && ! -L /etc/WindowServer.ini ]]; then
    cp -a /etc/WindowServer.ini "${PREFIX}/etc/WindowServer.ini"
    rm -f /etc/WindowServer.ini
    ln -s "${PREFIX}/etc/WindowServer.ini" /etc/WindowServer.ini
fi
if [[ -d /etc/serenity && ! -L /etc/serenity ]]; then
    cp -a /etc/serenity/. "${PREFIX}/etc/serenity/"
    rm -rf /etc/serenity
    ln -s "${PREFIX}/etc/serenity" /etc/serenity
fi

# WindowServer persists theme / wallpaper / font changes through
# /etc/WindowServer.ini, which is a symlink into ${PREFIX}/etc. The desktop
# session runs unprivileged, so the prefix copy has to be writable by that
# user - otherwise sync_config_to_disk() fails and switching the theme (e.g.
# "turn the theme off" back to the classic look) only lasts one session.
if [[ -d "${PREFIX}/etc" ]]; then
    chown -R "${SESSION_USER}:${SESSION_GROUP}" "${PREFIX}/etc"
    [[ -f "${PREFIX}/etc/WindowServer.ini" ]] && chmod 664 "${PREFIX}/etc/WindowServer.ini"
fi

# --- /res 指向 prefix 的资源（Serenity 代码里写死的路径）------------------
ln -sfn "${PREFIX}/res" /res

echo "== [5/6] 写 systemd unit =="

# --- systemd --------------------------------------------------------------
if [[ "${INSTALL_SYSTEMD}" == "1" && "${EUID}" -ne 0 ]]; then
    echo "!! 不是 root，跳过 unit 写入（要写 /etc/systemd/system 和 polkit 规则）" >&2
elif [[ "${INSTALL_SYSTEMD}" == "1" ]]; then
    cat > /etc/systemd/system/serenity-desktop.service <<EOF
[Unit]
Description=SerenityOS desktop session (serenitid)
# 替换 getty 与 plymouth-quit（同 gdm.service 的做法）：plymouthd 在退出前一直是
# DRM master，合成器拿不到 master 时 SetCrtc 只会得到 Permission denied —— 屏幕
# 上是看不到任何东西的，日志里也只是 "KMS presenter unavailable"。
Conflicts=getty@tty1.service kmsconvt@tty1.service plymouth-quit.service
After=rc-local.service plymouth-start.service systemd-user-sessions.service plymouth-quit.service
OnFailure=plymouth-quit.service

[Service]
Type=simple
# Run the desktop as an unprivileged user: file operations can no longer
# touch system files, and the session gets a real /run/user/<uid> so PipeWire
# (audio), D-Bus and XDG dirs initialise without hacks.
User=${SESSION_USER}
Group=${SESSION_GROUP}
# PAMName=login would give us a full logind session + XDG_RUNTIME_DIR for free,
# but it also fights with auto-login/seat management here, so we set the
# runtime dir explicitly and let serenitid's children inherit it. The uid is
# the session user's real one: on a normal install 1000 is already the human,
# so a hardcoded 1000 here would hand the desktop someone else's runtime dir.
Environment=XDG_RUNTIME_DIR=/run/user/${SESSION_UID}
Environment=HOME=${SESSION_HOME}
Environment=SERENITY_PREFIX=${PREFIX}
Environment=SERENITY_BIN=${PREFIX}/bin
Environment=PATH=${PREFIX}/bin:/usr/local/bin:/usr/bin:/bin
# serenitid/splash paths: progress lives in a RuntimeDirectory owned by the
# user; the log lands in /tmp (writable without root).
Environment=SERENITY_PROGRESS=/run/serenity/progress
Environment=SERENITY_LOG=/tmp/serenity-session.log
RuntimeDirectory=serenity
RuntimeDirectoryMode=0755
# Quit the boot splash first: it owns the DRM master until it goes away, and the
# WindowServer needs to be the one holding it. --retain-splash keeps the last
# frame on screen so there is no flash of black before the desktop paints.
ExecStartPre=+sh -c "plymouth quit --retain-splash 2>/dev/null || true"
# Clean up the classic root-owned socket dirs before the unprivileged session
# recreates them (the + prefix runs this as root). Without this the old
# root-owned /tmp/portal and /tmp/session/0 block the user from listening.
ExecStartPre=+sh -c "install -d -o ${SESSION_USER} -g ${SESSION_GROUP} -m 700 /run/user/${SESSION_UID}; rm -rf /tmp/portal /tmp/session"
ExecStart=${PREFIX}/bin/serenitid
# Ctrl+Alt+Del: the kernel sends SIGINT to /proc/sys/kernel/cad_pid. It defaults
# to PID 1 (systemd), which reboots the machine. serenitid installs a SIGINT
# handler that opens the task manager, but cad_pid is root:root 0600 and the
# session is unprivileged. This root-side hook points cad_pid at the real
# session process shortly after ExecStart, so CAD becomes "open task manager".
ExecStartPost=+sh -c "sleep 2; pgrep -x serenitid.real > /proc/sys/kernel/cad_pid"
# 真机测试期开着自动重启：serenitid 崩过一次（SIGSEGV，19:54），没有桌面的时候
# 屏幕只剩 console，没人能上去点。崩太快就由 StartLimit 收住，不会无限重启。
Restart=on-failure
RestartSec=2
StartLimitIntervalSec=60
StartLimitBurst=3
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
EOF

    # 注销之后的「关机/重启」由未特权的会话用户发起，logind 与 systemd 都要过
    # polkit；不放行时 systemctl 只会以 "Interactive authentication required"
    # 失败，而 GUI 里完全看不出来。只放行本会话用户和本会话单元。
    install -d /etc/polkit-1/rules.d
    cat > /etc/polkit-1/rules.d/50-serenity-session.rules <<POLKIT
/* Installed by scripts/port/05-install-prefix.sh: let the SerenityOS desktop
   session shut down, reboot and restart itself without a password prompt. */
polkit.addRule(function (action, subject) {
    if (subject.user != "${SESSION_USER}")
        return null;
    var unit = action.lookup("unit");
    if (action.id == "org.freedesktop.login1.power-off" ||
        action.id == "org.freedesktop.login1.power-off-multiple-sessions" ||
        action.id == "org.freedesktop.login1.reboot" ||
        action.id == "org.freedesktop.login1.reboot-multiple-sessions" ||
        (action.id == "org.freedesktop.systemd1.manage-units" && unit == "serenity-desktop.service"))
        return polkit.Result.YES;
    return null;
});
POLKIT
    # Belt-and-braces: even if the cad_pid hook above fails (kernel without the
    # sysctl, race, non-systemd boot), CAD must never reboot the machine.
    # serenitid's own SIGINT handler still fires when cad_pid does point at it.
    systemctl mask ctrl-alt-del.target >/dev/null 2>&1 || true
    systemctl daemon-reload
    # 一定要 enable：serenity-restore-gnome 会把 unit disable 掉，之后只重跑本脚本
    # 的话开机又进不了桌面，而「unit 写好了」的输出会让人以为已经生效。
    systemctl enable serenity-desktop.service
    # Presenter 选择放在 drop-in 里（unit 保持规范），这样换回来只需删一个文件。
    # wlroots 路径的前提：seatd 在跑、会话用户在 video 组；且 SERENITY_NO_CURSOR=1
    # 关掉 WindowServer 自绘光标（wlroots 画的那个才能盖在外来窗口上面）。
    if [[ "${PRESENTER}" == "wlroots" ]]; then
        install -d -m755 /etc/systemd/system/serenity-desktop.service.d
        cat > /etc/systemd/system/serenity-desktop.service.d/wlroots.conf <<EOF
[Service]
Environment=SERENITY_PRESENTER=wlroots
Environment=SERENITY_NO_CURSOR=1
EOF
        echo "presenter: wlroots（drop-in 已写；需要 seatd 且会话用户在 video 组）"
    else
        rm -f /etc/systemd/system/serenity-desktop.service.d/wlroots.conf
        echo "presenter: ${PRESENTER}（未启用 wlroots drop-in）"
    fi
    systemctl daemon-reload

    echo "已写入 /etc/systemd/system/serenity-desktop.service 并 enable"
fi

# --- 清理旧的 /usr/local 副本（可逆：挪到 prefix 下的备份目录）------------
if [[ "${CLEAN_OLD}" == "1" ]]; then
    backup="${PREFIX}/.old-usr-local-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "${backup}/bin" "${backup}/lib"
    shopt -s nullglob
    old_bins=(/usr/local/bin/serenity-* /usr/local/bin/serenitid)
    old_libs=(/usr/local/lib/liblagom-*)
    if (( ${#old_bins[@]} )); then
        mv "${old_bins[@]}" "${backup}/bin/"
    fi
    if (( ${#old_libs[@]} )); then
        mv "${old_libs[@]}" "${backup}/lib/"
    fi
    shopt -u nullglob
    ldconfig
    echo "旧副本已移到 ${backup}（确认无误后可删除）"
fi

echo
echo "完成。检查："
echo "  ls ${PREFIX}/bin | head"
echo "  readlink /res   # -> ${PREFIX}/res"
echo "  systemctl restart serenity-desktop.service"
echo "  ps -ef | grep serenity-shell"
