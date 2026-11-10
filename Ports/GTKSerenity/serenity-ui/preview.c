/* Headless reference renderer for the Serenity widget set.
 *
 *   serenity-widget-preview [--out FILE.png] [--theme "Redmond 2000"]
 *
 * Draws every widget kind into a cairo image surface, so the look of the
 * patched GTK can be checked without an X server, a Wayland compositor or a
 * running desktop. It is also the visual regression target used by check.sh.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "serenity-ui.h"
#include <cairo.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 960
#define H 780

static void set_c(cairo_t *cr, SerenityColor c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static void label(cairo_t *cr, double x, double y, const char *text,
                  SerenityColor c, int bold)
{
    cairo_save(cr);
    set_c(cr, c);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, bold ? 12.0 : 10.0);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    cairo_restore(cr);
}

static void box_label(cairo_t *cr, double x, double y, const char *text)
{
    label(cr, x, y, text, serenity_ui_color(SERENITY_ROLE_WINDOW_TEXT), 0);
}


/* ---- the one drawing path -------------------------------------------- *
 * Widgets are NOT drawn here any more: the real Gfx::StylePainter does it
 * (libserenitypaint.so) and we only blit the resulting BGRA pixels. The
 * headless preview therefore shows exactly what the patched GTK shows.
 */
typedef int (*paint_widget_fn)(int, int, unsigned char *, int, int, int,
                               double, int, int);
static paint_widget_fn s_paint_widget;
static int s_bridge_tried;

static void load_bridge(void)
{
    const char *paths[4];
    char local[512];
    int i;
    const char *prefix;

    if (s_bridge_tried) return;
    s_bridge_tried = 1;
    prefix = getenv("SERENITY_PREFIX");
    snprintf(local, sizeof local, "%s/lib/libserenitypaint.so", prefix ? prefix : "");
    paths[0] = getenv("SERENITY_PAINT_BRIDGE");
    paths[1] = local;
    paths[2] = "../build/stage/lib/libserenitypaint.so";
    paths[3] = "libserenitypaint.so";
    for (i = 0; i < 4; i++) {
        void *h;
        if (!paths[i] || !*paths[i]) continue;
        h = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
        if (!h) continue;
        s_paint_widget = (paint_widget_fn)dlsym(h, "serenity_paint_widget");
        if (s_paint_widget) return;
    }
    fprintf(stderr, "warning: libserenitypaint.so not found; widgets will be blank\n");
}

static void blit(cairo_t *cr, double x, double y, int w, int h, unsigned char *px)
{
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        px, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    cairo_save(cr);
    cairo_set_source_surface(cr, surf, x, y);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surf);
}

/* The compositor owns real window frames, so the title bar is the single
 * kind GTK never paints; the preview keeps a cairo stand-in for it. */
static void titlebar_cairo(cairo_t *cr, double x, double y, double w, double h,
                           SerenityWidgetState st, const char *text)
{
    int active = !(st & SERENITY_STATE_BACKDROP);
    SerenityColor c1 = serenity_ui_color(active ? SERENITY_ROLE_TITLE_ACTIVE_1
                                               : SERENITY_ROLE_TITLE_INACTIVE_1);
    SerenityColor c2 = serenity_ui_color(active ? SERENITY_ROLE_TITLE_ACTIVE_2
                                               : SERENITY_ROLE_TITLE_INACTIVE_2);
    SerenityColor fg = serenity_ui_color(active ? SERENITY_ROLE_TITLE_TEXT
                                               : SERENITY_ROLE_TITLE_INACTIVE_TEXT);
    cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x + w, y);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, c1.r, c1.g, c1.b, 1.0);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, c2.r, c2.g, c2.b, 1.0);
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    if (text && *text) {
        cairo_save(cr);
        set_c(cr, fg);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, x + 6, y + h / 2.0 + 4);
        cairo_show_text(cr, text);
        cairo_restore(cr);
    }
}

static void pw(cairo_t *cr, double x, double y, double w, double h,
               SerenityWidgetKind kind, SerenityWidgetState st,
               const SerenityWidgetExtra *ex)
{
    int iw, ih;
    unsigned char *px;

    if (kind == SERENITY_WIDGET_WINDOW_TITLEBAR) {
        titlebar_cairo(cr, x, y, w, h, st, ex ? ex->text : NULL);
        return;
    }
    load_bridge();
    if (!s_paint_widget) return;
    iw = (int)w;
    ih = (int)h;
    if (iw < 1 || ih < 1) return;
    px = (unsigned char *)calloc((size_t)iw * ih, 4);
    if (!px) return;
    if (s_paint_widget(iw, ih, px, (int)kind, (int)st,
                       ex ? ex->vertical : 0,
                       ex ? ex->value : 0.0,
                       ex ? ex->max_value : 0,
                       ex ? (int)ex->arrow : 0) == 0)
        blit(cr, x, y, iw, ih, px);
    free(px);
}

int main(int argc, char **argv)
{
    const char *out = "/tmp/serenity-widgets.png";
    int i;
    cairo_surface_t *surface;
    cairo_t *cr;
    SerenityColor text = { 0, 0, 0, 1 };
    SerenityColor desktop = { 0, 0, 0, 1 };
    SerenityWidgetExtra ex = { SERENITY_ARROW_NONE, 0.45, 0, 0, NULL };

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            /* Honoured through SERENITY_SOURCE_ROOT + WindowServer.ini;
             * kept as a documented hook for the test harness. */
            (void)argv[++i];
        }
    }

    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    text = serenity_ui_color(SERENITY_ROLE_WINDOW_TEXT);
    desktop = serenity_ui_color_by_name("DesktopBackground");
    if (desktop.a == 0.0) desktop = serenity_ui_color(SERENITY_ROLE_WINDOW);

    /* desktop background */
    set_c(cr, desktop);
    cairo_paint(cr);
    /* a "window" panel holding the sample widgets */
    pw(cr, 16, 16, W - 32, H - 32,
                          SERENITY_WIDGET_FRAME_RAISED, SERENITY_STATE_NORMAL, NULL);
    pw(cr, 18, 18, W - 36, 30,
                          SERENITY_WIDGET_WINDOW_TITLEBAR, SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_NONE, 0, 0, 0,
                              "SerenityUI widget set" });

    /* ---- buttons ---------------------------------------------------- */
    label(cr, 36, 70, "Buttons (normal / hover / pressed / disabled / focused / default)",
          text, 1);
    pw(cr, 36, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 172, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_HOVER, NULL);
    pw(cr, 308, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_PRESSED, NULL);
    pw(cr, 444, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_DISABLED, NULL);
    pw(cr, 580, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_FOCUSED, NULL);
    pw(cr, 716, 80, 120, 26, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_DEFAULT | SERENITY_STATE_FOCUSED, NULL);
    {
        const char *names[] = { "OK", "Hover", "Pressed", "Disabled", "Focused", "Default" };
        for (i = 0; i < 6; i++)
            label(cr, 36 + i * 136 + 8, 98, names[i],
                  serenity_ui_color(SERENITY_ROLE_FACE_TEXT), 0);
    }

    /* ---- entry / combo ---------------------------------------------- */
    label(cr, 36, 136, "Entry / spin / combo", text, 1);
    pw(cr, 36, 146, 260, 26, SERENITY_WIDGET_ENTRY,
                          SERENITY_STATE_NORMAL, NULL);
    label(cr, 44, 164, "The quick brown fox", serenity_ui_color(SERENITY_ROLE_BASE_TEXT), 0);
    pw(cr, 312, 146, 260, 26, SERENITY_WIDGET_ENTRY,
                          SERENITY_STATE_DISABLED, NULL);
    label(cr, 320, 164, "disabled", serenity_ui_color(SERENITY_ROLE_DISABLED_TEXT), 0);
    pw(cr, 588, 146, 240, 26, SERENITY_WIDGET_COMBO,
                          SERENITY_STATE_NORMAL, NULL);
    label(cr, 596, 164, "Serenity", serenity_ui_color(SERENITY_ROLE_FACE_TEXT), 0);
    ex.arrow = SERENITY_ARROW_DOWN;
    pw(cr, 802, 146, 26, 26, SERENITY_WIDGET_COMBO,
                          SERENITY_STATE_NORMAL, &ex);
    ex.arrow = SERENITY_ARROW_NONE;

    /* ---- check / radio ---------------------------------------------- */
    label(cr, 36, 202, "Check boxes / radio buttons", text, 1);
    for (i = 0; i < 4; i++) {
        SerenityWidgetState st = SERENITY_STATE_NORMAL;
        if (i & 1) st |= SERENITY_STATE_CHECKED;
        if (i == 2) st |= SERENITY_STATE_DISABLED;
        pw(cr, 36 + i * 120, 212, 30, 24,
                              SERENITY_WIDGET_CHECK, st, NULL);
        pw(cr, 36 + i * 120 + 40, 212, 30, 24,
                              SERENITY_WIDGET_RADIO, st, NULL);
    }
    box_label(cr, 36, 248, "unchecked / checked / disabled / pressed");

    /* ---- frames / group box ----------------------------------------- */
    label(cr, 520, 202, "Frames (raised / sunken / group box)", text, 1);
    pw(cr, 520, 212, 120, 60, SERENITY_WIDGET_FRAME_RAISED,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 656, 212, 120, 60, SERENITY_WIDGET_FRAME_SUNKEN,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 792, 212, 132, 60, SERENITY_WIDGET_GROUP_BOX,
                          SERENITY_STATE_NORMAL, NULL);
    label(cr, 528, 246, "raised", text, 0);
    label(cr, 664, 246, "sunken", text, 0);
    label(cr, 800, 246, "group box", text, 0);

    /* ---- scrollbars -------------------------------------------------- */
    label(cr, 36, 290, "Scrollbars (trough + thumb + steppers)", text, 1);
    pw(cr, 36, 300, 420, 17,
                          SERENITY_WIDGET_SCROLLBAR_TROUGH, SERENITY_STATE_NORMAL, NULL);
    pw(cr, 36, 300, 17, 17, SERENITY_WIDGET_SCROLLBAR_BUTTON,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_LEFT, 0, 0, 0, NULL });
    pw(cr, 130, 300, 130, 17, SERENITY_WIDGET_SCROLLBAR_THUMB,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 439, 300, 17, 17, SERENITY_WIDGET_SCROLLBAR_BUTTON,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_RIGHT, 0, 0, 0, NULL });

    pw(cr, 480, 300, 17, 150,
                          SERENITY_WIDGET_SCROLLBAR_TROUGH, SERENITY_STATE_NORMAL, NULL);
    pw(cr, 480, 300, 17, 17, SERENITY_WIDGET_SCROLLBAR_BUTTON,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_UP, 0, 1, 0, NULL });
    pw(cr, 480, 345, 17, 60, SERENITY_WIDGET_SCROLLBAR_THUMB,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 480, 433, 17, 17, SERENITY_WIDGET_SCROLLBAR_BUTTON,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_DOWN, 0, 1, 0, NULL });

    /* ---- progress ---------------------------------------------------- */
    label(cr, 540, 290, "Progress bar", text, 1);
    pw(cr, 540, 300, 340, 22,
                          SERENITY_WIDGET_PROGRESS_TROUGH, SERENITY_STATE_NORMAL, NULL);
    ex.value = 0.45;
    pw(cr, 542, 302, 336, 18,
                          SERENITY_WIDGET_PROGRESS_FILL, SERENITY_STATE_NORMAL, &ex);
    label(cr, 540, 340, "45% complete", text, 0);

    /* ---- tabs / toolbar / status ------------------------------------- */
    label(cr, 540, 370, "Tabs / toolbar / status bar", text, 1);
    for (i = 0; i < 3; i++)
        pw(cr, 540 + i * 90, 380, 88, 24,
                              i == 0 ? SERENITY_WIDGET_TAB_ACTIVE : SERENITY_WIDGET_TAB,
                              SERENITY_STATE_NORMAL, NULL);
    label(cr, 552, 396, "General", text, 0);
    label(cr, 642, 396, "View", text, 0);
    label(cr, 732, 396, "Options", text, 0);
    pw(cr, 540, 410, 340, 26, SERENITY_WIDGET_TOOLBAR,
                          SERENITY_STATE_NORMAL, NULL);
    for (i = 0; i < 4; i++)
        pw(cr, 546 + i * 44, 414, 38, 18,
                              SERENITY_WIDGET_BUTTON, SERENITY_STATE_NORMAL, NULL);
    label(cr, 540, 460, "Status bar:", text, 0);
    pw(cr, 620, 446, 260, 20, SERENITY_WIDGET_STATUS_BAR,
                          SERENITY_STATE_NORMAL, NULL);
    label(cr, 628, 460, "Ready", text, 0);

    /* ---- menu -------------------------------------------------------- */
    label(cr, 36, 350, "Menu / menu items", text, 1);
    pw(cr, 36, 360, 200, 116, SERENITY_WIDGET_MENU,
                          SERENITY_STATE_NORMAL, NULL);
    for (i = 0; i < 5; i++) {
        SerenityWidgetState st = SERENITY_STATE_NORMAL;
        if (i == 2) st = SERENITY_STATE_SELECTED;
        pw(cr, 39, 363 + i * 22, 194, 21,
                              SERENITY_WIDGET_MENU_ITEM, st, NULL);
        label(cr, 68, 377 + i * 22,
              i == 0 ? "  New" : i == 1 ? "  Open..." : i == 2 ? "  Save" :
              i == 3 ? "  ----" : "  Exit",
              serenity_ui_color(i == 2 ? SERENITY_ROLE_MENU_SELECTION_TEXT
                                      : SERENITY_ROLE_MENU_TEXT), 0);
    }

    /* ---- handle / separator ----------------------------------------- */
    label(cr, 260, 350, "Handle / separators", text, 1);
    pw(cr, 260, 360, 120, 18, SERENITY_WIDGET_HANDLE,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 260, 392, 200, 2, SERENITY_WIDGET_SEPARATOR,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 260, 410, 2, 60, SERENITY_WIDGET_SEPARATOR,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_NONE, 0, 1, 0, NULL });
    label(cr, 280, 440, "vertical / horizontal", text, 0);

    /* ---- title bars -------------------------------------------------- */
    label(cr, 36, 500, "Title bars (active / inactive)", text, 1);
    pw(cr, 36, 510, 400, 24, SERENITY_WIDGET_WINDOW_TITLEBAR,
                          SERENITY_STATE_NORMAL,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_NONE, 0, 0, 0,
                              "Document - WordCraft" });
    pw(cr, 36, 542, 400, 24, SERENITY_WIDGET_WINDOW_TITLEBAR,
                          SERENITY_STATE_BACKDROP,
                          &(SerenityWidgetExtra){ SERENITY_ARROW_NONE, 0, 0, 0,
                              "Document - WordCraft" });

    /* ---- header bar -------------------------------------------------- */
    label(cr, 470, 500, "Header bar (libadwaita repainted as a toolbar)", text, 1);
    pw(cr, 470, 510, 454, 32, SERENITY_WIDGET_HEADER_BAR,
                          SERENITY_STATE_NORMAL, NULL);
    pw(cr, 478, 516, 76, 20, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_NORMAL, NULL);
    label(cr, 486, 530, "Open", serenity_ui_color(SERENITY_ROLE_FACE_TEXT), 0);
    pw(cr, 846, 516, 70, 20, SERENITY_WIDGET_BUTTON,
                          SERENITY_STATE_DEFAULT, NULL);
    label(cr, 854, 530, "Save", serenity_ui_color(SERENITY_ROLE_FACE_TEXT), 0);

    /* ---- theme info -------------------------------------------------- */
    {
        char info[512];
        snprintf(info, sizeof(info),
                 "theme: %s   |   font: %s %dpt   |   bevel: %dpx   radius: %dpx   scrollbar: %dpx",
                 serenity_ui_theme_name(), serenity_ui_font_family(),
                 serenity_ui_font_size(),
                 serenity_ui_metric(SERENITY_METRIC_BEVEL),
                 serenity_ui_metric(SERENITY_METRIC_RADIUS),
                 serenity_ui_metric(SERENITY_METRIC_SCROLLBAR_W));
        label(cr, 36, 600, info, text, 1);
        label(cr, 36, 620, "All colours above are read live from the active Serenity SystemTheme.",
              text, 0);
        label(cr, 36, 638, "This is the exact rendering used by the patched GTK3 / GTK4 / libadwaita.",
              text, 0);
        snprintf(info, sizeof(info), "source: %s", serenity_ui_theme_path());
        label(cr, 36, 656, info, text, 0);
    }

    /* ---- palette swatches ------------------------------------------- */
    label(cr, 36, 690, "SystemTheme palette", text, 1);
    for (i = 0; i < SERENITY_ROLE_COUNT; i++) {
        double sx = 36 + (i % 17) * 52;
        double sy = 700 + (i / 17) * 34;
        char h[8];
        serenity_ui_color_hex((SerenityRole)i, h);
        set_c(cr, serenity_ui_color((SerenityRole)i));
        cairo_rectangle(cr, sx, sy, 46, 20);
        cairo_fill(cr);
        set_c(cr, serenity_ui_color(SERENITY_ROLE_SHADOW));
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, sx + 0.5, sy + 0.5, 45, 19);
        cairo_stroke(cr);
        label(cr, sx, sy + 32, h, text, 0);
    }

    cairo_destroy(cr);
    if (cairo_surface_write_to_png(surface, out) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "failed to write %s\n", out);
        cairo_surface_destroy(surface);
        return 1;
    }
    cairo_surface_destroy(surface);
    printf("%s\n", out);
    return 0;
}
