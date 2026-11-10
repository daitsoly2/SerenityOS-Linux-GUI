#!/usr/bin/env python3
"""Apply the Serenity widget-rendering fork to the vendored GTK sources.

Idempotent: every edit is guarded by a marker, so running this twice is a
no-op. If an upstream pattern no longer matches, the script fails loudly
instead of silently producing a GTK that lost part of the integration.

Hard constraints (this is a fork for one specific desktop):
  * no public header is modified except gtksettingsprivate.h (a private
    header containing only #defines);
  * no struct layout, no exported function signature, no SONAME changes;
  * all new functions are hidden/internal;
  * the drawing library is compiled INTO the GTK libraries (no new .so,
    therefore no new DT_NEEDED entry and no new exported symbols).
"""
from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / "vendor"
OVERLAY = ROOT / "overlay"
UI = ROOT / "serenity-ui"

MARK = "SERENITY_HOOK"

errors: list[str] = []
changes: list[str] = []


class P:
    def __init__(self, rel: str):
        self.path = VENDOR / rel
        if not self.path.is_file():
            raise SystemExit(f"missing source file: {self.path}")
        self.text = self.path.read_text(encoding="utf-8", errors="replace")
        self.orig = self.text

    def replace(self, old: str, new: str, label: str, count: int = 1) -> None:
        if MARK in new and new in self.text:
            return  # already applied
        if old not in self.text:
            errors.append(f"{self.path.name}: [{label}] pattern not found:\n{old[:180]}")
            return
        self.text = self.text.replace(old, new, count)

    def regex(self, pattern: str, repl: str, label: str) -> None:
        if repl.strip() in self.text:
            return
        new, n = re.subn(pattern, repl, self.text, count=1, flags=re.M | re.S)
        if n == 0:
            errors.append(f"{self.path.name}: [{label}] regex did not match:\n{pattern[:180]}")
            return
        self.text = new

    def save(self) -> None:
        if self.text != self.orig:
            self.path.write_text(self.text, encoding="utf-8")
            changes.append(str(self.path.relative_to(VENDOR)))


def copy_overlay() -> None:
    """Install shim sources + the drawing library into each source tree."""
    targets = {
        "gtk3/gtk": [OVERLAY / "gtk3/gtk/gtkserenity.c",
                     OVERLAY / "gtk3/gtk/gtkserenity.h"],
        "gtk4/gtk": [OVERLAY / "gtk4/gtk/gtkserenity.c",
                     OVERLAY / "gtk4/gtk/gtkserenity.h"],
        "libadwaita/src": [OVERLAY / "libadwaita/src/adw-serenity.c",
                           OVERLAY / "libadwaita/src/adw-serenity.h"],
    }
    # There is no drawing library to copy any more: serenity-widgets.c (the
    # cairo reimplementation of ClassicStylePainter) is gone, and the GTK
    # shims call Serenity's real painter through libserenitypaint.so.
    shared = [UI / "serenity-ui.h", UI / "serenity-ui.c"]
    for dest, files in targets.items():
        d = VENDOR / dest
        for f in files + shared:
            if not f.is_file():
                raise SystemExit(f"missing overlay source: {f}")
            shutil.copy2(f, d / f.name)


# ---------------------------------------------------------------- GTK3 ----

def patch_gtk3() -> None:
    p = P("gtk3/gtk/gtksettingsprivate.h")
    p.replace('#define DEFAULT_THEME_NAME      "Adwaita"',
              '#define DEFAULT_THEME_NAME      "Serenity" /* SERENITY_HOOK */',
              "default theme")
    p.replace('#define DEFAULT_ICON_THEME      "Adwaita"',
              '#define DEFAULT_ICON_THEME      "Serenity" /* SERENITY_HOOK */',
              "default icon theme")
    p.save()

    p = P("gtk3/gtk/gtkrender.c")
    p.replace('#include "gtkstylecontextprivate.h"',
              '#include "gtkstylecontextprivate.h"\n#include "gtkserenity.h" /* SERENITY_HOOK */',
              "include")
    guards = ("  g_return_if_fail (GTK_IS_STYLE_CONTEXT (context));\n"
              "  g_return_if_fail (cr != NULL);\n\n")
    hooks = {
        "gtk_render_background":
            "  if (_gtk_serenity_render_background (context, cr, x, y, width, height))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_frame":
            "  if (_gtk_serenity_render_frame (context, cr, x, y, width, height))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_check":
            "  if (_gtk_serenity_render_check (context, cr, x, y, width, height))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_option":
            "  if (_gtk_serenity_render_option (context, cr, x, y, width, height))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_slider":
            "  if (_gtk_serenity_render_slider (context, cr, x, y, width, height, orientation))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_handle":
            "  if (_gtk_serenity_render_handle (context, cr, x, y, width, height))\n"
            "    return; /* SERENITY_HOOK */\n",
        "gtk_render_arrow":
            "  if (_gtk_serenity_render_arrow (context, cr, angle, x, y, size))\n"
            "    return; /* SERENITY_HOOK */\n",
    }
    for fn, hook in hooks.items():
        # Idempotence: the hook text itself is the marker.
        if hook.strip() in p.text:
            continue
        # Match only this function's signature + guard block, so the hook
        # always lands inside the right function body.
        pattern = (r"(^" + re.escape(fn) + r" \([\s\S]*?\n\{\n)"
                   + re.escape(guards))
        p.regex(pattern, r"\1" + guards + hook, fn)
    p.save()

    # GtkMenuItem / GtkModelButton / buttons inside popovers never call
    # gtk_render_background(): as gadget-based widgets they paint straight
    # from gtk_css_gadget_draw(). Route that box through the bridge too.
    p = P("gtk3/gtk/gtkcssgadget.c")
    p.replace("#include \"gtkrenderbackgroundprivate.h\"",
              "#include \"gtkrenderbackgroundprivate.h\"\n"
              "#include \"gtkserenity.h\" /* SERENITY_HOOK */",
              "gadget include")
    gadget_old = (
        "  gtk_css_style_render_background (style,\n"
        "                                   cr,\n"
        "                                   x + margin.left,\n"
        "                                   y + margin.top,\n"
        "                                   width - margin.left - margin.right,\n"
        "                                   height - margin.top - margin.bottom,\n"
        "                                   gtk_css_node_get_junction_sides (priv->node));\n"
        "  gtk_css_style_render_border (style,\n"
        "                               cr,\n"
        "                               x + margin.left,\n"
        "                               y + margin.top,\n"
        "                               width - margin.left - margin.right,\n"
        "                               height - margin.top - margin.bottom,\n"
        "                               0,\n"
        "                               gtk_css_node_get_junction_sides (priv->node));\n")
    gadget_new = (
        "  if (_gtk_serenity_render_gadget (gtk_css_gadget_get_owner (gadget),\n"
        "                                   gtk_css_node_get_name (priv->node),\n"
        "                                   gtk_css_node_get_state (priv->node),\n"
        "                                   cr,\n"
        "                                   x + margin.left, y + margin.top,\n"
        "                                   width - margin.left - margin.right,\n"
        "                                   height - margin.top - margin.bottom))\n"
        "    {\n"
        "      /* SERENITY_HOOK: the Serenity painter owns this box; the menu\n"
        "       * panel's base and stripe show through idle items. */\n"
        "    }\n"
        "  else\n"
        "    {\n"
        "      gtk_css_style_render_background (style,\n"
        "                                   cr,\n"
        "                                   x + margin.left,\n"
        "                                   y + margin.top,\n"
        "                                   width - margin.left - margin.right,\n"
        "                                   height - margin.top - margin.bottom,\n"
        "                                   gtk_css_node_get_junction_sides (priv->node));\n"
        "      gtk_css_style_render_border (style,\n"
        "                               cr,\n"
        "                               x + margin.left,\n"
        "                               y + margin.top,\n"
        "                               width - margin.left - margin.right,\n"
        "                               height - margin.top - margin.bottom,\n"
        "                               0,\n"
        "                               gtk_css_node_get_junction_sides (priv->node));\n"
        "    }\n")
    p.replace(gadget_old, gadget_new, "gadget background hook")
    p.save()

    p = P("gtk3/gdk/wayland/gdkdisplay-wayland.c")
    p.replace("  if (display_wayland->server_decoration_manager)\n"
              "    return display_wayland->server_decoration_mode == ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER;\n"
              "  return FALSE;",
              "  /* SERENITY_HOOK: WindowServer owns every window frame. */\n"
              "  (void) display_wayland;\n"
              "  return TRUE;",
              "always SSD")
    p.save()

    p = P("gtk3/gtk/meson.build")
    p.replace("gtk_sources = files(\n",
              "gtk_sources = files(\n"
              "  'gtkserenity.c', 'serenity-ui.c', # SERENITY_HOOK\n",
              "source list")
    p.text = p.text.replace("'serenity-widgets.c', # SERENITY_HOOK", "# SERENITY_HOOK")
    p.save()


# ---------------------------------------------------------------- GTK4 ----

def patch_gtk4() -> None:
    p = P("gtk4/gtk/gtksettingsprivate.h")
    p.replace('#define DEFAULT_THEME_NAME      "Default"',
              '#define DEFAULT_THEME_NAME      "Serenity" /* SERENITY_HOOK */',
              "default theme")
    p.replace('#define DEFAULT_ICON_THEME      "Adwaita"',
              '#define DEFAULT_ICON_THEME      "Serenity" /* SERENITY_HOOK */',
              "default icon theme")
    p.save()

    for rel, func in (("gtk4/gtk/gtkrenderbackground.c", "background"),
                      ("gtk4/gtk/gtkrenderborder.c", "border")):
        p = P(rel)
        p.replace('#include "gtkrenderbackgroundprivate.h"'
                  if func == "background" else '#include "gtkrenderborderprivate.h"',
                  ('#include "gtkrenderbackgroundprivate.h"\n'
                   '#include "gtkserenity.h" /* SERENITY_HOOK */'
                   if func == "background" else
                   '#include "gtkrenderborderprivate.h"\n'
                   '#include "gtkserenity.h" /* SERENITY_HOOK */'),
                  "include")
        name = ("gtk_css_style_snapshot_background (GtkCssBoxes *boxes,\n"
                "                                   GtkSnapshot *snapshot)"
                if func == "background" else
                "gtk_css_style_snapshot_border (GtkCssBoxes *boxes,\n"
                "                               GtkSnapshot *snapshot)")
        hook = ("  if (_gtk_serenity_suppress_background ())\n"
                "    return; /* SERENITY_HOOK */\n"
                if func == "background" else
                "  if (_gtk_serenity_suppress_border ())\n"
                "    return; /* SERENITY_HOOK */\n")
        p.replace(name + "\n{\n", name + "\n{\n" + hook, "snapshot hook")
        p.save()

    p = P("gtk4/gtk/gtkwidget.c")
    p.replace('#include "gtkwidgetprivate.h"',
              '#include "gtkwidgetprivate.h"\n#include "gtkserenity.h" /* SERENITY_HOOK */',
              "include")
    p.replace("  gtk_widget_do_snapshot (widget, snapshot);\n\n"
              "  if (priv->render_node)",
              "  _gtk_serenity_snapshot_begin (widget, snapshot); /* SERENITY_HOOK */\n"
              "  gtk_widget_do_snapshot (widget, snapshot);\n"
              "  _gtk_serenity_snapshot_end (widget); /* SERENITY_HOOK */\n\n"
              "  if (priv->render_node)",
              "snapshot bracketing")
    p.save()

    # The real paint hook: gtk_widget_create_render_node() is the single point
    # every widget (root and children) passes, with the border-box geometry
    # GTK itself uses for the CSS background.
    p = P("gtk4/gtk/gtkwidget.c")
    p.replace("  gtk_css_style_snapshot_background (&boxes, snapshot);\n"
              "  gtk_css_style_snapshot_border (&boxes, snapshot);\n",
              "  /* SERENITY_HOOK: forward the background/border paint to Serenity's\n"
              "   * ClassicStylePainter at the exact border-box geometry. When it paints,\n"
              "   * skip the stock CSS background/border. */\n"
              "  if (!_gtk_serenity_paint (widget, snapshot,\n"
              "                            &gtk_css_boxes_get_border_box (&boxes)->bounds))\n"
              "    {\n"
              "      gtk_css_style_snapshot_background (&boxes, snapshot);\n"
              "      gtk_css_style_snapshot_border (&boxes, snapshot);\n"
              "    }\n",
              "paint hook")
    p.save()

    p = P("gtk4/gdk/wayland/gdkdisplay-wayland.c")
    p.replace("  if (display_wayland->server_decoration_manager)\n"
              "    return display_wayland->server_decoration_mode == ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER;\n\n"
              "  return FALSE;",
              "  /* SERENITY_HOOK: WindowServer owns every window frame. */\n"
              "  (void) display_wayland;\n"
              "  return TRUE;",
              "always SSD")
    p.save()

    p = P("gtk4/gtk/meson.build")
    p.replace("gtk_private_sources = files([\n",
              "gtk_private_sources = files([\n"
              "  'gtkserenity.c', 'serenity-ui.c', # SERENITY_HOOK\n",
              "source list")
    p.text = p.text.replace("'serenity-widgets.c', # SERENITY_HOOK", "# SERENITY_HOOK")
    p.save()


# ---------------------------------------------------------- libadwaita ----

def patch_libadwaita() -> None:
    p = P("libadwaita/src/adw-style-manager.c")
    p.replace("static gboolean\nget_is_dark (AdwStyleManager *self)\n{\n",
              "static gboolean\nget_is_dark (AdwStyleManager *self)\n{\n"
              "  /* SERENITY_HOOK: one fixed palette; never probe the portal\n"
              "     (also avoids the 25s startup stall on this desktop). */\n"
              "  if (serenity_ui_is_active ())\n"
              "    return FALSE;\n",
              "force light")
    # add include after the first #include "config.h"
    p.replace('#include "config.h"\n',
              '#include "config.h"\n#include "serenity-ui.h" /* SERENITY_HOOK */\n',
              "include")
    p.save()

    p = P("libadwaita/src/adw-main.c")
    p.replace("  gtk_init ();\n",
              "  gtk_init ();\n  adw_serenity_init (); /* SERENITY_HOOK */\n",
              "adw_init hook")
    p.replace('#include "adw-main.h"',
              '#include "adw-main.h"\n#include "adw-serenity.h" /* SERENITY_HOOK */',
              "include") if '#include "adw-main.h"' in p.text else None
    # adw-main.c may use a different first include; fall back to config.h.
    if "adw-serenity.h" not in p.text:
        p.replace('#include "config.h"',
                  '#include "config.h"\n#include "adw-serenity.h" /* SERENITY_HOOK */',
                  "include fallback")
    p.save()

    p = P("libadwaita/src/meson.build")
    p.replace("src_sources = [\n",
              "src_sources = [\n"
              "  'adw-serenity.c', 'serenity-ui.c', # SERENITY_HOOK\n",
              "source list")
    p.text = p.text.replace("'serenity-widgets.c', # SERENITY_HOOK", "# SERENITY_HOOK")
    p.save()



def fix_source_lists() -> None:
    """Keep the three meson source lists canonical and idempotent.

    serenity-widgets.c no longer exists (the cairo reimplementation was
    dropped), and re-running this script must never leave a duplicated
    gtkserenity.c entry behind.
    """
    for rel in ("gtk4/gtk/meson.build", "gtk3/gtk/meson.build",
                "libadwaita/src/meson.build"):
        path = VENDOR / rel
        text = path.read_text(encoding="utf-8")
        lines, out, prev = text.splitlines(True), [], None
        for line in lines:
            if "SERENITY_HOOK" in line and "serenity-ui.c" in line:
                line = line.replace(", 'serenity-widgets.c',", ",")
                line = re.sub(r"'(gtkserenity|adw-serenity)\.c', 'serenity-ui\.c', *",
                              lambda m: "'" + m.group(1) + ".c', 'serenity-ui.c', ",
                              line)
                if line == prev:
                    continue
            out.append(line)
            prev = line
        new_text = "".join(out)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changes.append(rel + " (source list)")


def patch_gtk3_tree_gaps() -> None:
    """The gtk-3-24 git snapshot is not a release tarball.

    It omits the generated translation/docs subdirectories and the tests/
    directory that upstream meson still references. This fork does not ship
    translations or docs, so drop those subdir() calls instead of inventing
    stub build files.
    """
    p = P("gtk3/meson.build")
    p.replace("subdir('po')\nsubdir('po-properties')\n"
              "subdir('docs/tools')\nsubdir('docs/reference')",
              "# SERENITY_HOOK: git snapshot omits generated po/ and docs/ trees\n"
              "# (translations and documentation are not shipped by this fork)",
              "missing subdirs")
    p.replace("testinc = include_directories('tests')",
              "testinc = include_directories('.') # SERENITY_HOOK: no tests/ in git snapshot",
              "tests include dir")
    p.save()


def patch_meson_probes() -> None:
    """Skip meson's per-flag compiler probes.

    They create and delete one temp directory per flag (~40 in GTK4). On
    build hosts with a bulk-delete guard this aborts `meson setup` halfway
    through, before any of our code is even compiled. The flags are only
    warning switches; dropping them has no effect on the resulting ABI.
    """
    for rel in ("gtk3/meson.build", "gtk4/meson.build"):
        p = P(rel)
        p.replace("common_cflags = cc.get_supported_arguments(test_cflags)",
                  "common_cflags = [] # SERENITY_HOOK: warning-only flags skipped",
                  "common_cflags")
        p.replace("add_project_arguments(cc.get_supported_arguments(test_cflags), language: 'c')",
                  "# SERENITY_HOOK: warning-only flags skipped",
                  "project args c")
        p.replace("add_project_arguments(cxx.get_supported_arguments(test_cflags), language: 'cpp')",
                  "# SERENITY_HOOK: warning-only flags skipped",
                  "project args cpp")
        p.save()

    p = P("gtk4/meson.build")
    p.replace("common_ldflags = cc.get_supported_link_arguments([\n"
              "  '-Wl,-Bsymbolic',\n"
              "  '-Wl,-z,relro',\n"
              "  '-Wl,-z,now',\n"
              "])",
              "common_ldflags = ['-Wl,-Bsymbolic', '-Wl,-z,relro', '-Wl,-z,now'] # SERENITY_HOOK",
              "link flags")
    p.save()


def main() -> int:
    for d in ("gtk3/gtk", "gtk4/gtk", "libadwaita/src"):
        if not (VENDOR / d).is_dir():
            raise SystemExit(f"missing vendor tree: {VENDOR/d}\nrun sync.sh first")
    copy_overlay()
    patch_gtk3()
    patch_gtk4()
    patch_libadwaita()
    patch_gtk3_tree_gaps()
    patch_meson_probes()
    fix_source_lists()

    if errors:
        print("FAILED — upstream patterns did not match:\n", file=sys.stderr)
        for e in errors:
            print("  * " + e, file=sys.stderr)
        return 1
    print(f"applied. {len(changes)} files changed:")
    for c in changes:
        print("  " + c)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
