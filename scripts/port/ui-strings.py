#!/usr/bin/env python3
"""Report UI translation coverage for the Chinese locale.

LibGUI's translate_ui() looks every widget string up in
Base/res/locale/<lang>.ini under [UI]. This script scans the sources for the
literals that actually reach those setters and tells you which ones are still
missing from the table.

    scripts/port/ui-strings.py            # 覆盖率 + 出现最多的未翻译串
    scripts/port/ui-strings.py --all      # 列出全部未翻译串
    scripts/port/ui-strings.py --app FileManager
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCALE = ROOT / "Base/res/locale/zh-CN.ini"

SCAN_ROOTS = [
    "Userland/Applications",
    "Userland/Demos",
    "Userland/Games",
    "Userland/Applets",
    "Ports/LinuxGUI",
]

# 只关心会把字符串显示出来的调用。
UI_CALL = re.compile(
    r"(set_text|set_title|add_submenu|add_tab|Action::create|Action::create_checkable|"
    r"set_tooltip|Label::create|GroupBox|MessageBox::show|set_column_name)"
)

STRING = re.compile(r'"((?:[^"\\]|\\.){2,80})"')

# 明显不是文案的：图标 URI、路径、纯格式串、纯标点。
NOT_TEXT = re.compile(r"^(resource://|/|[a-z0-9_-]+://)")


def load_keys() -> set[str]:
    keys: set[str] = set()
    if not LOCALE.exists():
        return keys
    group = ""
    for line in LOCALE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
            continue
        if group != "UI" or "=" not in line:
            continue
        keys.add(line.split("=", 1)[0].strip())
    return keys


def is_candidate(text: str) -> bool:
    if NOT_TEXT.match(text):
        return False
    if "\\" in text:  # 转义序列（\n、\t、\"）在 ini 里表达不了，也不值得翻译
        return False
    if not re.search(r"[A-Za-z]", text):
        return False
    if not re.search(r"[A-Za-z]{2,}", text):  # 单个字母/占位符
        return False
    if text in {"{}", "%Y-%m-%d"}:
        return False
    return True


def pattern_keys(keys: set[str]) -> list[tuple[str, str]]:
    """把 "Score: {}" 这样的键拆成 (前缀, 后缀)，用于匹配格式化后的文本。"""
    result = []
    for key in keys:
        hole = key.find("{}")
        if hole < 0 or key.find("{}", hole + 2) >= 0:
            continue
        result.append((key[:hole], key[hole + 2:]))
    return result


def covered_by_pattern(text: str, patterns: list[tuple[str, str]]) -> bool:
    for prefix, suffix in patterns:
        if len(text) > len(prefix) + len(suffix) and text.startswith(prefix) and text.endswith(suffix):
            return True
    return False


def scan(path_filter: str | None) -> collections.Counter[str]:
    found: collections.Counter[str] = collections.Counter()
    for root in SCAN_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for path in base.rglob("*.cpp"):
            if path_filter and path_filter.lower() not in str(path).lower():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for line in text.splitlines():
                if not UI_CALL.search(line):
                    continue
                for match in STRING.finditer(line):
                    literal = match.group(1)
                    if is_candidate(literal):
                        found[literal] += 1
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true", help="列出全部未翻译串")
    parser.add_argument("--app", help="只看路径里包含这个名字的源文件")
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args()

    keys = load_keys()
    patterns = pattern_keys(keys)
    found = scan(args.app)
    if not found:
        print("没有扫到候选串（检查路径？）")
        return 1

    missing = collections.Counter(
        {
            k: v
            for k, v in found.items()
            if k not in keys
            and k.replace("&", "") not in keys
            and not covered_by_pattern(k, patterns)
        }
    )
    total = sum(found.values())
    covered = total - sum(missing.values())
    print(f"[UI] 表条目：{len(keys)}")
    print(f"源码里的候选串：{len(found)} 个不同串 / {total} 处出现")
    print(f"已覆盖：{covered}/{total} 处（{covered * 100 // max(total, 1)}%），"
          f"未覆盖 {len(missing)} 个不同串")

    if missing:
        print()
        print("未翻译（按出现次数）：" if not args.all else "未翻译（全部）：")
        items = missing.most_common() if args.all else missing.most_common(args.limit)
        for text, count in items:
            print(f"  {count:3d}  {text}")
        if not args.all and len(missing) > args.limit:
            print(f"  ... 还有 {len(missing) - args.limit} 个，用 --all 查看")
    return 0


if __name__ == "__main__":
    sys.exit(main())
