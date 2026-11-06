#!/usr/bin/env bash
# 可选：在 QEMU 里启动一个干净的 Debian 13 云镜像，用于验证移植。
#
# 依赖：qemu-system-x86、qemu-utils、cloud-image-utils
#   sudo apt-get install -y qemu-system-x86 qemu-utils cloud-image-utils
#
# 用法：
#   ./04-qemu-debian13.sh            # 串口控制台（-nographic）
#   ./04-qemu-debian13.sh --vnc      # VNC，默认 :1 -> localhost:5901
#
# 登录：用户 serenity / 密码 serenity（仅本地测试用）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM="${ROOT}/Build/vm"
mkdir -p "${VM}"

BASE="${VM}/debian-13-genericcloud-amd64.qcow2"
DISK="${VM}/debian13-port.qcow2"
SEED="${VM}/seed.iso"
IMAGE_URL="${DEBIAN_CLOUD_IMAGE:-https://cloud.debian.org/images/cloud/trixie/latest/debian-13-genericcloud-amd64.qcow2}"

if [[ ! -f "${BASE}" ]]; then
    echo "== 下载 Debian 13 云镜像 =="
    curl -L --fail --retry 3 --progress-bar -o "${BASE}" "${IMAGE_URL}"
fi

if [[ ! -f "${DISK}" ]]; then
    echo "== 创建差分层磁盘 =="
    qemu-img create -f qcow2 -b "${BASE}" -F qcow2 "${DISK}" 40G
fi

if [[ ! -f "${SEED}" ]]; then
    if ! command -v cloud-localds >/dev/null 2>&1; then
        echo "需要 cloud-localds（apt-get install cloud-image-utils）" >&2
        exit 1
    fi

    cat > "${VM}/user-data" <<'EOF'
#cloud-config
hostname: serenity-port
users:
  - name: serenity
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: false
chpasswd:
  list: |
    serenity:serenity
  expire: false
ssh_pwauth: true
package_update: true
packages:
  - build-essential
  - clang
  - lld
  - cmake
  - ninja-build
  - pkg-config
  - libsdl2-dev
  - git
  - curl
EOF
    echo "instance-id: serenity-port" > "${VM}/meta-data"
    cloud-localds "${SEED}" "${VM}/user-data" "${VM}/meta-data"
fi

ACCEL=()
if [[ -e /dev/kvm ]]; then
    ACCEL=(-enable-kvm -cpu host)
else
    echo "未检测到 /dev/kvm，使用 TCG 软件模拟（较慢）"
    ACCEL=(-cpu max)
fi

MODE="${1:-}"
if [[ "${MODE}" == "--vnc" ]]; then
    exec qemu-system-x86_64 "${ACCEL[@]}" -m 4096 -smp 4 \
        -drive file="${DISK}",if=virtio \
        -drive file="${SEED}",format=raw,if=virtio \
        -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
        -vnc :1
else
    exec qemu-system-x86_64 "${ACCEL[@]}" -m 4096 -smp 4 \
        -drive file="${DISK}",if=virtio \
        -drive file="${SEED}",format=raw,if=virtio \
        -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
        -nographic
fi
