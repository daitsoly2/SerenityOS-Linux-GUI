#!/usr/bin/env bash
# 一次性把整机切到 Serenity 桌面（或切回 GNOME），并自己判断成败。
#
#   sudo scripts/port/07-switch-to-serenity.sh            # 切到 Serenity
#   sudo scripts/port/07-switch-to-serenity.sh --restore  # 切回 GNOME
#
# 为什么用脚本而不是手敲两条 systemctl：在 GNOME 的终端里 `stop gdm3` 会连自己
# 这个终端一起杀掉，后面那条 `start serenity-desktop` 根本不会执行（19:56 那次
# 就是这么「切了但没过去」的）。所以这个脚本建议这样起，彻底脱离会话：
#
#   sudo systemd-run --collect --unit serenity-switch \
#        /mnt/user/serenity/scripts/port/07-switch-to-serenity.sh
#
# 报告同时写到 /tmp/serenity-switch-report.txt，切回来之后可以直接看。
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
PREFIX="${PREFIX:-/serenity}"
REPORT=/tmp/serenity-switch-report.txt
SESSION_LOG=/tmp/serenity-session.log
RESTORE=0
[[ "${1:-}" == "--restore" ]] && RESTORE=1

exec > >(tee "${REPORT}") 2>&1

say() { echo "[$(date '+%F %T')] $*"; }

restore_gnome()
{
    say "== 切回 GNOME =="
    systemctl disable --now serenity-desktop.service 2>/dev/null
    systemctl enable --now gdm3 2>/dev/null
    systemctl set-default graphical.target 2>/dev/null
    say "已在 3 秒后恢复 gdm3（如果屏幕没回来，Ctrl+Alt+F2）"
    sleep 3
}

if [[ "${EUID}" -ne 0 ]]; then
    say "需要 root"
    exit 1
fi

if [[ "${RESTORE}" == "1" ]]; then
    restore_gnome || true
    say "完成"
    exit 0
fi

# 1. 让显示管理器松手：它活着就是 DRM master，合成器抢不到屏幕。
#    先只 stop，不 disable —— 失败时脚本会把它拉回来。
if systemctl is-active --quiet gdm3.service; then
    say "== 停掉 gdm3（当前会话会消失，这是预期的）=="
    systemctl stop gdm3.service || say "!! 停 gdm3 失败"
fi
# plymouth 若还在，它同样握着 master。
plymouth quit --retain-splash 2>/dev/null && say "已退出 plymouth"

# 2. 上一次的失败状态要清掉，否则 start 可能直接被 StartLimit 拒。
systemctl reset-failed serenity-desktop.service 2>/dev/null
systemctl daemon-reload

say "== 启动 serenity-desktop =="
if ! systemctl start serenity-desktop.service; then
    say "!! systemctl start 直接失败"
fi

# 3. 等到窗口服务器把 socket 建出来（最多 15 秒）。
for _ in $(seq 1 30); do
    [[ -S /tmp/portal/window ]] && break
    sleep 0.5
done

sleep 1

# 4. 判定：进程活着 + KMS 真的接管了屏幕（SDL 回退＝屏幕上看不见）。
active=$(systemctl is-active serenity-desktop.service)
kms=$(grep -c "KMS presenter on" "${SESSION_LOG}" 2>/dev/null || echo 0)
say "serenity-desktop: ${active}   KMS presenter 出现次数: ${kms}"

say "== systemctl status（截断）=="
systemctl status serenity-desktop.service --no-pager -l 2>&1 | head -25

say "== 会话日志尾部 =="
tail -25 "${SESSION_LOG}" 2>/dev/null

if [[ "${active}" == "active" && "${kms}" -gt 0 ]]; then
    say "== 成功：桌面应该已经在屏幕上了 =="
    exit 0
fi

say "!! 失败：桌面进程没拿到 KMS（屏幕上看不到东西）。原因看上面日志里的"
say "!!   'KMS presenter unavailable (...)' / 'cannot take over the display'"
restore_gnome
say "!! 已自动切回 GNOME，报告留在 ${REPORT}"
exit 1
