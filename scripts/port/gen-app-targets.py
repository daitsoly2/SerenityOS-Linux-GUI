#!/usr/bin/env python3
"""Generate host build targets for SerenityOS GUI applications.

Walks Userland/{Applications,Games,Demos}, reads each CMakeLists.txt and emits an
equivalent add_executable() block for the Lagom (host) build. Apps whose link
dependencies are not available in Lagom (e.g. LibWeb) are skipped and reported.

Usage: gen-app-targets.py <project-root> <output.cmake>
"""

import os
import re
import sys

LIB_PREFIX = "Lib"

# Libraries that exist in the Lagom build (Meta/Lagom/CMakeLists.txt
# lagom_standard_libraries plus a few explicitly added ones).
AVAILABLE_LIBS = {
    "AccelGfx", "ACPI", "Archive", "Audio", "Compress", "Config", "Crypto",
    "Cpp", "DeviceTree", "DNS", "Diff", "Gemini", "Gfx", "GL", "GLSL", "GPU",
    "HID", "HTTP", "IMAP", "ImageDecoderClient", "IPC", "JIT", "JS", "Keyboard",
    "Line", "Locale", "Manual", "Markdown", "PDF", "Media", "Protocol", "Regex",
    "RIFF", "SoftGPU", "Shell", "SQL", "SSH", "Syntax", "TextCodec", "Threading",
    "TLS", "Unicode", "URL", "Wasm", "WebSocket", "XML",
    # Built specifically for the host GUI port:
    "Core", "CoreBasic", "CoreMinimal", "FileSystem", "GUI", "GUI_GML", "IPC",
    "Desktop", "Main", "ELF",
    # EDID is a pure-AK parser; the Serenity-only bits are guarded so it
    # compiles on the host and reads EDID from a Linux sysfs file instead.
    "EDID",
    # Added to Lagom so the remaining bundled apps can be hosted:
    "Cards", "Chess", "CMake", "Disassembly", "DSP",
    "Maps", "VT", "FileSystemAccessClient",
}

TARGET_PATTERNS = [
    re.compile(r"serenity_app\(\s*(\w+)", re.S),
    re.compile(r"serenity_gui_app\(\s*(\w+)", re.S),
    re.compile(r"serenity_bin\(\s*(\w+)", re.S),
]


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_list_block(text, name):
    """Extract a `set(NAME ...)` block and return its entries."""
    m = re.search(r"set\(\s*" + name + r"\b(.*?)\)\s*\n", text, re.S)
    if not m:
        return []
    body = m.group(1)
    items = []
    for token in body.split():
        if token.startswith("${") or token.startswith("#"):
            continue
        if token.endswith(".cpp") or token.endswith(".h"):
            items.append(token)
    return items


def parse_gml(text):
    return re.findall(r"compile_gml\(\s*(\S+)\s+(\S+)(?:\s+\S+)?\s*\)", text)


def parse_stringify_gml(text):
    # stringify_gml(source.gml OutputGML.h string_name) -> embed_as_string --type=string-view
    return re.findall(r"stringify_gml\(\s*(\S+)\s+(\S+)\s+(\S+)\s*\)", text)


def parse_libs(text):
    libs = set()
    for m in re.finditer(r"target_link_libraries\((.*?)\)", text, re.S):
        for token in m.group(1).split():
            if token in ("PRIVATE", "PUBLIC", "INTERFACE", "${TARGET}") or token.startswith("$"):
                continue
            if token.startswith(LIB_PREFIX):
                libs.add(token[3:])
    return libs


def app_kind(rel_dir):
    top = rel_dir.split("/")[1] if rel_dir.startswith("Userland/") else "Applications"
    return {"Applications": "app", "Games": "game", "Demos": "demo", "Applets": "applet"}.get(top, "app")


def main():
    root = os.path.abspath(sys.argv[1])
    out_path = sys.argv[2]
    app_root = os.path.join(root, "Userland")

    built, skipped = [], []

    for group in ("Applications", "Games", "Demos", "Applets"):
        group_dir = os.path.join(app_root, group)
        if not os.path.isdir(group_dir):
            continue
        for name in sorted(os.listdir(group_dir)):
            app_dir = os.path.join(group_dir, name)
            cmake = os.path.join(app_dir, "CMakeLists.txt")
            if not os.path.isfile(cmake):
                continue

            text = read(cmake)

            target = None
            for pattern in TARGET_PATTERNS:
                m = pattern.search(text)
                if m:
                    target = m.group(1)
                    break
            if not target:
                skipped.append((name, "no serenity_app/serenity_bin target"))
                continue

            libs = parse_libs(text)
            missing = sorted(libs - AVAILABLE_LIBS)
            if missing:
                skipped.append((name, "needs " + ", ".join("Lib" + m for m in missing)))
                continue

            sources = parse_list_block(text, "SOURCES")
            generated = parse_list_block(text, "GENERATED_SOURCES")
            gml = parse_gml(text)
            stringify = parse_stringify_gml(text)

            if not sources:
                skipped.append((name, "no explicit SOURCES list"))
                continue

            # Needs a GPU / shader stack and trips -Werror on the host build.
            if name in ("3DFileViewer", "Audio"):
                skipped.append((name, "host build incompatible (GPU / audio backend)"))
                continue

            # AnalogClock is built explicitly by Apps/CMakeLists.txt (bring-up target).
            if name == "AnalogClock":
                skipped.append((name, "built explicitly in Apps/CMakeLists.txt"))
                continue

            # Screenshot is replaced by the port's own Screenshot & Recorder
            # panel (Ports/LinuxGUI/ScreenshotApp), which drives the compositor
            # recorder instead of shelling out to the unbuilt /bin/shot.
            if name == "Screenshot":
                skipped.append((name, "built explicitly in Apps/CMakeLists.txt (recorder panel)"))
                continue

            # VideoPlayer is replaced by the port's libmpv-backed player
            # (Ports/LinuxGUI/VideoPlayer). The upstream LibMedia backend only
            # handles VP8/VP9 in Matroska, so MP4/AVI/MOV could not be played.
            if name == "VideoPlayer":
                skipped.append((name, "built explicitly in Apps/CMakeLists.txt (libmpv player)"))
                continue

            # Drop .h files, they are not compiled directly.
            sources = [s for s in sources if s.endswith(".cpp")]
            generated = [g for g in generated if g.endswith(".cpp")]

            built.append({
                "name": name,
                "target": target,
                "dir": app_dir,
                "kind": app_kind(os.path.join("Userland", group, name)),
                "sources": sources,
                "generated": generated,
                "gml": gml,
                "stringify": stringify,
                "libs": sorted(libs),
            })

    lines = [
        "# Generated by scripts/port/gen-app-targets.py -- do not edit by hand.",
        "# %d host app targets, %d skipped." % (len(built), len(skipped)),
        "",
        "# Compile a GML file from an app directory into the build tree.",
        "function(port_compile_gml app_dir source output)",
        "    add_custom_command(",
        "        OUTPUT ${output}",
        "        COMMAND sh -c \"$<TARGET_FILE:Lagom::GMLCompiler> '${app_dir}/${source}' > '${output}'\"",
        "        DEPENDS Lagom::GMLCompiler ${app_dir}/${source}",
        "        VERBATIM",
        "    )",
        "endfunction()",
        "",
        "# Stringify a GML file into a *GML.h header (embed_as_string --type=string-view).",
        "function(port_stringify_gml app_dir source output string_name)",
        "    add_custom_command(",
        "        OUTPUT ${output}",
        "        COMMAND \"${Python3_EXECUTABLE}\" \"${SERENITY_PROJECT_ROOT}/Meta/embed_as_string.py\" \"${app_dir}/${source}\" --type=string-view -o \"${output}.tmp\" -n \"${string_name}\"",
        "        COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"${output}.tmp\" \"${output}\"",
        "        COMMAND \"${CMAKE_COMMAND}\" -E remove \"${output}.tmp\"",
        "        DEPENDS \"${SERENITY_PROJECT_ROOT}/Meta/embed_as_string.py\"",
        "        MAIN_DEPENDENCY \"${app_dir}/${source}\"",
        "        VERBATIM",
        "    )",
        "endfunction()",
        "",
    ]

    all_targets = []
    for app in built:
        sanitized = app["target"].replace(".", "-")
        target = "serenity-" + re.sub(r"(?<!^)(?=[A-Z])", "-", sanitized).lower()
        app["cmake_target"] = target
        all_targets.append(target)

        lines.append("# %s (%s)" % (app["name"], app["kind"]))
        rel_dir = os.path.relpath(app["dir"], root)
        lines.append("set(%s_DIR \"${SERENITY_PROJECT_ROOT}/%s\")" % (app["name"], rel_dir))

        gml_outputs = []
        gml_generated_names = {output for _, output in app["gml"]}
        for source, output in app["gml"]:
            out_abs = "${CMAKE_CURRENT_BINARY_DIR}/%s/%s" % (app["name"], output)
            lines.append("port_compile_gml(${%s_DIR} %s %s)" % (app["name"], source, out_abs))
            gml_outputs.append(out_abs)

        # stringify_gml -> *GML.h headers, emitted under ${CMAKE_BINARY_DIR}/Userland
        # so that `#include <Applications/<App>/X.h>` resolves via include_directories(${CMAKE_BINARY_DIR}).
        stringify_outputs = []
        for source, output, string_name in app["stringify"]:
            out_abs = "${CMAKE_BINARY_DIR}/Userland/%s/%s" % (
                os.path.relpath(app["dir"], os.path.join(root, "Userland")).replace("\\", "/"), output)
            lines.append("port_stringify_gml(${%s_DIR} %s %s %s)" % (app["name"], source, out_abs, string_name))
            stringify_outputs.append(out_abs)

        lines.append("add_executable(%s" % target)
        for source in app["sources"]:
            if source in app["generated"] or source in gml_generated_names:
                continue
            lines.append("    ${%s_DIR}/%s" % (app["name"], source))
        for output in gml_outputs:
            lines.append("    " + output)
        lines.append(")")
        lines.append("target_include_directories(%s PRIVATE ${%s_DIR})" % (target, app["name"]))
        if stringify_outputs:
            # So that `#include <Applications/<App>/XGML.h>` finds stringified headers
            # emitted under ${CMAKE_BINARY_DIR}/Userland.
            lines.append("target_include_directories(%s PRIVATE ${CMAKE_BINARY_DIR}/Userland)" % target)

        # Ensure stringified headers exist before any TU is compiled.
        if stringify_outputs:
            lines.append("add_custom_target(%s-stringify-headers DEPENDS %s)" % (target, " ".join(stringify_outputs)))
            lines.append("add_dependencies(%s %s-stringify-headers)" % (target, target))

        link_libs = ["Lib" + lib for lib in app["libs"] if lib not in ("GUI_GML",)]
        lines.append("target_link_libraries(%s PRIVATE %s)" % (
            target, " ".join(link_libs) if link_libs else "LibCore"))
        lines.append("set_target_properties(%s PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/bin\")" % target)
        lines.append("")

    lines.append("set(PORT_APP_TARGETS")
    for target in all_targets:
        lines.append("    " + target)
    lines.append(")")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("generated %d targets -> %s" % (len(built), out_path))
    for name, reason in skipped:
        print("  skipped %-24s %s" % (name, reason))


if __name__ == "__main__":
    main()
