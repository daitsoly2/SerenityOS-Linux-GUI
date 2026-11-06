#!/usr/bin/env python3
"""Install a CJK font into a Wine prefix so Chinese Windows apps stop showing
tofu (empty boxes) instead of characters.

Why this is needed
------------------
Wine asks GDI for the font an app names ("SimSun", "Microsoft YaHei", ...).
If that name is not present it falls back to a Latin face (Liberation Sans and
friends) which has no CJK glyphs, so every Chinese character renders as a box.
Installing a font file alone is not enough: the name still has to resolve.

What this script does
---------------------
  1. finds a CJK font (Sarasa Gothic SC by default, then Noto Sans CJK SC,
     then WenQuanYi Micro Hei),
  2. copies it into <prefix>/drive_c/windows/Fonts/,
  3. writes a .reg that registers the face, maps the common Chinese font names
     to it through FontSubstitutes, and adds FontLink entries so Latin UI fonts
     fall back to it for CJK,
  4. imports the .reg with `wine reg import` (falls back to `regedit /S`).

Usage
-----
  sudo scripts/port/wine-fonts.py                       # /root/.wine-dw
  scripts/port/wine-fonts.py --prefix ~/.wine --dry-run
  scripts/port/wine-fonts.py --font /path/to/font.ttf
  scripts/port/wine-fonts.py --no-reg                   # copy + write .reg only

Exit status is 0 when the font is in place (and imported, unless --no-reg).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Preferred fonts, most suitable first. Sarasa Gothic SC is a single-face TTF
# that also carries Latin, which is exactly what the rest of this port uses.
FONT_CANDIDATES = [
    "Sarasa Gothic SC",
    "Sarasa Term SC",
    "Noto Sans CJK SC",
    "Source Han Sans SC",
    "WenQuanYi Micro Hei",
]

# Windows font names Chinese apps ask for. Both the English and the localized
# names are used in the wild (a lot of software hardcodes 宋体 / 微软雅黑).
SUBSTITUTES = [
    "SimSun", "宋体",
    "NSimSun", "新宋体",
    "SimHei", "黑体",
    "Microsoft YaHei", "微软雅黑", "Microsoft YaHei UI",
    "KaiTi", "楷体", "KaiTi_GB2312",
    "FangSong", "仿宋", "FangSong_GB2312",
    "Microsoft JhengHei", "微軟正黑體",
    "MS Shell Dlg", "MS Shell Dlg 2",
]

# Latin UI fonts that should fall back to the CJK font for CJK code points,
# without changing how Latin text looks.
FONTLINK_FONTS = [
    "Tahoma",
    "Arial",
    "MS Sans Serif",
    "Microsoft Sans Serif",
    "Segoe UI",
    "Verdana",
    "Times New Roman",
    "Courier New",
    "System",
]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def find_font(explicit: str | None) -> tuple[Path, str]:
    """Return (font file, family name)."""
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_file():
            raise SystemExit(f"font not found: {path}")
        return path, font_family(path)

    if not shutil.which("fc-match"):
        raise SystemExit("fontconfig (fc-match) is missing; pass --font explicitly")

    for family in FONT_CANDIDATES:
        result = run(["fc-match", "-f", "%{file}", family])
        candidate = result.stdout.strip()
        if result.returncode == 0 and candidate and Path(candidate).is_file():
            return Path(candidate), font_family(Path(candidate))

    raise SystemExit(
        "no CJK font found; install one of: " + ", ".join(FONT_CANDIDATES)
    )


def font_family(path: Path) -> str:
    """Ask fontconfig for the first family name of a font file."""
    if shutil.which("fc-query"):
        result = run(["fc-query", "-f", "%{family[0]}", str(path)])
        name = result.stdout.strip()
        if result.returncode == 0 and name:
            return name
    # Fall back to the file stem; good enough for SarasaGothicSC-Regular.ttf.
    return path.stem.split("-")[0]


def multi_sz(entries: list[str]) -> str:
    """Encode REG_MULTI_SZ as the hex(7): form used in .reg files."""
    data = b""
    for entry in entries:
        data += entry.encode("utf-16-le") + b"\x00\x00"
    data += b"\x00\x00"
    return "hex(7):" + ",".join(f"{byte:02x}" for byte in data)


def reg_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def build_reg(font_filename: str, family: str) -> str:
    windows_path = f"C:\\windows\\Fonts\\{font_filename}"
    link_entry = f"{windows_path},{family}"

    lines = [
        "Windows Registry Editor Version 5.00",
        "",
        r"[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\Fonts]",
        f'"{reg_escape(family)} (TrueType)"="{reg_escape(windows_path)}"',
        "",
        r"[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes]",
    ]
    for name in SUBSTITUTES:
        lines.append(f'"{reg_escape(name)}"="{reg_escape(family)}"')

    lines += [
        "",
        r"[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink]",
    ]
    encoded = multi_sz([link_entry])
    for name in FONTLINK_FONTS:
        lines.append(f'"{reg_escape(name)}"={encoded}')

    return "\r\n".join(lines) + "\r\n"


FONTS_KEY = r"HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\Fonts"
SUBS_KEY = r"HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes"
LINK_KEY = r"HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink"


def reg_add(environment: dict, key: str, name: str, type_: str, data: str) -> bool:
    command = ["wine", "reg", "add", key, "/v", name, "/t", type_, "/d", data, "/f"]
    try:
        result = subprocess.run(command, env=environment, capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  reg add {name!r} failed: {error}", file=sys.stderr)
        return False
    if result.returncode != 0:
        print(f"  reg add {name!r} exited with {result.returncode}", file=sys.stderr)
        return False
    return True


def apply_reg(font_filename: str, family: str, prefix: Path) -> bool:
    """Write the font registration with `wine reg add`, one value at a time.

    `wine reg import` silently drops keys whose names are non-ASCII (e.g. 宋体),
    and on a full file it can skip whole sections - so the FontSubstitutes above
    never took effect. Per-value `reg add` writes each one explicitly and the
    Chinese-named aliases actually land.
    """
    environment = os.environ.copy()
    environment["WINEPREFIX"] = str(prefix)
    environment.setdefault("WINEDEBUG", "-all")
    environment.setdefault("LANG", "zh_CN.UTF-8")
    environment.setdefault("LC_ALL", "zh_CN.UTF-8")

    if not shutil.which("wine"):
        return False

    windows_path = f"C:\\windows\\Fonts\\{font_filename}"
    ok = True

    # 1. Register the face itself.
    if not reg_add(environment, FONTS_KEY, f"{family} (TrueType)", "REG_SZ", windows_path):
        ok = False

    # 2. Map every Chinese font name apps ask for to our face. This is what
    #    stops the tofu boxes: without it "SimSun"/"宋体" resolve to a Latin face.
    for name in SUBSTITUTES:
        if not reg_add(environment, SUBS_KEY, name, "REG_SZ", family):
            ok = False

    # 3. FontLink so Latin UI fonts fall back to the CJK face for CJK glyphs.
    #    REG_MULTI_SZ is best-effort (some wine builds reject /d for it).
    for name in FONTLINK_FONTS:
        reg_add(environment, LINK_KEY, name, "REG_MULTI_SZ", f"{windows_path},{family}")

    return ok


def prefix_looks_valid(prefix: Path) -> bool:
    return (prefix / "drive_c" / "windows").is_dir()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", default=os.environ.get("WINEPREFIX") or "/root/.wine-dw",
                        help="Wine prefix (default: $WINEPREFIX or /root/.wine-dw)")
    parser.add_argument("--font", help="explicit font file to install")
    parser.add_argument("--no-reg", action="store_true", help="copy the font and write the .reg, but do not import it")
    parser.add_argument("--dry-run", action="store_true", help="show what would happen, change nothing")
    args = parser.parse_args()

    prefix = Path(args.prefix).expanduser()
    if not prefix_looks_valid(prefix):
        raise SystemExit(
            f"{prefix} does not look like a Wine prefix (missing drive_c/windows).\n"
            "Create it first:  WINEPREFIX={0} wineboot -i".format(prefix)
        )

    font_path, family = find_font(args.font)
    fonts_dir = prefix / "drive_c" / "windows" / "Fonts"
    target = fonts_dir / font_path.name
    reg_path = prefix / "serenity-fonts.reg"
    marker = prefix / ".serenity-cjk-font"

    print(f"prefix : {prefix}")
    print(f"font   : {font_path}  (family: {family})")
    print(f"target : {target}")

    if args.dry_run:
        print("\n--dry-run: not copying or importing")
        print(build_reg(font_path.name, family))
        return 0

    fonts_dir.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.stat().st_size != font_path.stat().st_size:
        # Copy next to the target first: a half-written font file makes GDI
        # fail in confusing ways, and a full disk is a real failure mode on
        # small /root partitions.
        temporary = target.with_name(target.name + ".tmp")
        try:
            shutil.copy2(font_path, temporary)
            os.replace(temporary, target)
        except OSError as error:
            temporary.unlink(missing_ok=True)
            raise SystemExit(f"failed to copy font into {fonts_dir}: {error}")
        print("copied font into the prefix")
    else:
        print("font already present in the prefix")

    reg_path.write_text(build_reg(font_path.name, family), encoding="utf-16")
    print(f"wrote {reg_path}")

    if args.no_reg:
        marker.write_text(f"{family}\n", encoding="utf-8")
        print("--no-reg: skipping import")
        return 0

    print("importing registry entries...")
    if not apply_reg(font_path.name, family, prefix):
        print(
            "could not run wine. Import manually once a display is available:\n"
            f"  WINEPREFIX={prefix} wine reg import {reg_path}",
            file=sys.stderr,
        )
        return 1

    marker.write_text(f"{family}\n", encoding="utf-8")
    print("done: Chinese font names now resolve to", family)
    return 0


if __name__ == "__main__":
    sys.exit(main())
