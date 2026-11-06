#!/usr/bin/env bash
# 预置 SerenityOS Lagom 构建所需的离线数据缓存。
#
# 说明：CMake 的 file(ARCHIVE_EXTRACT) 在部分沙箱/overlay 文件系统上会因
# "Can't restore time" 失败，所以这里提前下载并解压好，构建时用
# -DENABLE_NETWORK_DOWNLOADS=OFF 直接复用。
#
# GitHub 资源统一走 gh-proxy 加速（可用 GH_PROXY 环境变量覆盖）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${ROOT}/Build/caches"
GH_PROXY="${GH_PROXY:-https://v4.gh-proxy.org}"

mkdir -p "${CACHE}"

log() { printf '\033[1;34m[fetch]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

gh() { printf '%s/%s' "${GH_PROXY}" "$1"; }

# download <url> <dest> [sha256]
download() {
    local url="$1" dest="$2" sha="${3:-}"

    if [[ -f "${dest}" ]]; then
        if [[ -z "${sha}" ]] || [[ "$(sha256sum "${dest}" | cut -d' ' -f1)" == "${sha}" ]]; then
            log "cached  ${dest}"
            return 0
        fi
    fi

    log "download ${url}"
    curl -L --fail --retry 3 --retry-delay 2 --progress-bar -o "${dest}" "${url}"

    if [[ -n "${sha}" ]]; then
        local actual
        actual="$(sha256sum "${dest}" | cut -d' ' -f1)"
        [[ "${actual}" == "${sha}" ]] || die "SHA256 校验失败: ${dest}\n  期望 ${sha}\n  实际 ${actual}"
    fi
}

# ---------------------------------------------------------------- CACERT
log "== CACERT =="
mkdir -p "${CACHE}/CACERT"
download "https://curl.se/ca/cacert-2026-10-20.pem" \
    "${CACHE}/CACERT/cacert-2026-10-20.pem" \
    "796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe"
printf '2026-10-20\n' > "${CACHE}/CACERT/version.txt"

# ---------------------------------------------------------------- TZDB
log "== TZDB =="
mkdir -p "${CACHE}/TZDB"
download "https://data.iana.org/time-zones/releases/tzdata2024a.tar.gz" \
    "${CACHE}/TZDB/tzdb.tar.gz" \
    "0d0434459acbd2059a7a8da1f3304a84a86591f6ed69c6248fffa502b6edffe3"
if [[ ! -f "${CACHE}/TZDB/africa" ]]; then
    log "extract TZDB"
    tar -xzf "${CACHE}/TZDB/tzdb.tar.gz" -C "${CACHE}/TZDB" --touch
fi
printf '2024a\n' > "${CACHE}/TZDB/version.txt"

# ---------------------------------------------------------------- UCD
log "== UCD =="
mkdir -p "${CACHE}/UCD"
download "https://www.unicode.org/Public/18.0.0/ucd/UCD.zip" \
    "${CACHE}/UCD/UCD.zip" \
    "7b3e555514060b92290d154f53655c5eb0fa62b16eb04c03434ff72d1a66a0d8"
if [[ ! -f "${CACHE}/UCD/UnicodeData.txt" ]]; then
    log "extract UCD"
    unzip -oq "${CACHE}/UCD/UCD.zip" -d "${CACHE}/UCD"
fi
download "https://www.unicode.org/Public/18.0.0/emoji/emoji-test.txt" \
    "${CACHE}/UCD/emoji-test.txt" \
    "8f3735cda1f92a779d78af67cf86066bb1f07143dc22f2ac29394d9bc57ab21a"
download "https://www.unicode.org/Public/18.0.0/idna/IdnaMappingTable.txt" \
    "${CACHE}/UCD/IdnaMappingTable.txt" \
    "a03b1eb38032268c696406a83f0972d6a815acd2c8d4151d42ec0fda70ffced1"
printf '18.0.0\n' > "${CACHE}/UCD/version.txt"
printf '18.0.0\n' > "${CACHE}/UCD/emoji-version.txt"
printf '18.0.0\n' > "${CACHE}/UCD/idna-version.txt"

# ---------------------------------------------------------------- CLDR
log "== CLDR (via gh-proxy) =="
mkdir -p "${CACHE}/CLDR"
download "$(gh "https://github.com/unicode-org/cldr-json/releases/download/45.0.0/cldr-45.0.0-json-modern.zip")" \
    "${CACHE}/CLDR/cldr.zip" \
    "ba934cdd40ad4fb6439004c7e746bef97fe2b597db1040fcaa6c7d0647742c1b"
if [[ ! -d "${CACHE}/CLDR/cldr-core" ]]; then
    log "extract CLDR"
    unzip -oq "${CACHE}/CLDR/cldr.zip" -d "${CACHE}/CLDR"
fi
printf '45.0.0\n' > "${CACHE}/CLDR/version.txt"

# ---------------------------------------------------------------- PublicSuffix
log "== PublicSuffix (via gh-proxy) =="
mkdir -p "${CACHE}/PublicSuffix"
download "$(gh "https://raw.githubusercontent.com/publicsuffix/list/master/public_suffix_list.dat")" \
    "${CACHE}/PublicSuffix/public_suffix_list.dat"

# ---------------------------------------------------------------- PNP IDs
log "== PNP IDs =="
download "https://uefi.org/uefi-pnp-export" "${CACHE}/pnp_ids.csv"

echo
log "全部缓存就绪：${CACHE}"
echo "下一步：./02-build.sh"
