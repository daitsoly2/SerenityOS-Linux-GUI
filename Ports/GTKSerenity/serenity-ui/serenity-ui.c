/* serenity-ui.c — read the SerenityOS SystemTheme and expose it to GTK.
 *
 * Pure libc + pthread. No GLib/GObject dependency: the three GTK forks link
 * this library privately (hidden visibility), so no new symbol ever leaks
 * into libgtk-3.so.0 / libgtk-4.so.1 / libadwaita-1.so.0.
 */
#define _POSIX_C_SOURCE 200809L
#include "serenity-ui.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_ENTRIES 256
#define KEY_LEN 64
#define VAL_LEN 160

typedef struct {
    char key[KEY_LEN];
    char value[VAL_LEN];
} IniEntry;

typedef struct {
    int initialized;
    int active;
    char theme_name[128];
    char config_path[512];
    char theme_path[512];
    IniEntry colors[MAX_ENTRIES];
    int color_count;
    char font_family[128];
    int font_size;
    int metrics[SERENITY_METRIC_COUNT];
    uint64_t revision;
    struct timespec config_stamp;
    struct timespec theme_stamp;
    char *css3;
    char *css4;
} ThemeState;

static ThemeState S;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* Fallback palette: "Redmond 2000" — used when no Serenity theme is
 * installed (e.g. building on a plain Linux host). */
static const struct {
    SerenityRole role;
    const char *key;
    const char *fallback;
} s_roles[SERENITY_ROLE_COUNT] = {
    { SERENITY_ROLE_FACE,                 "Button",                   "#d4d0c8" },
    { SERENITY_ROLE_FACE_TEXT,            "ButtonText",               "#000000" },
    { SERENITY_ROLE_WINDOW,               "Window",                   "#d4d0c8" },
    { SERENITY_ROLE_WINDOW_TEXT,          "WindowText",               "#000000" },
    { SERENITY_ROLE_BASE,                 "Base",                     "#ffffff" },
    { SERENITY_ROLE_BASE_TEXT,            "BaseText",                 "#000000" },
    { SERENITY_ROLE_HIGHLIGHT,            "ThreedHighlight",          "#ffffff" },
    { SERENITY_ROLE_LIGHT,                "ThreedShadow1",            "#808080" },
    { SERENITY_ROLE_SHADOW,               "ThreedShadow2",            "#404040" },
    { SERENITY_ROLE_SELECTION,            "Selection",                "#1a3584" },
    { SERENITY_ROLE_SELECTION_TEXT,       "SelectionText",            "#ffffff" },
    { SERENITY_ROLE_MENU,                 "MenuBase",                 "#ffffff" },
    { SERENITY_ROLE_MENU_TEXT,            "MenuBaseText",             "#000000" },
    { SERENITY_ROLE_MENU_SELECTION,       "MenuSelection",            "#b6c0d2" },
    { SERENITY_ROLE_MENU_SELECTION_TEXT,  "MenuSelectionText",        "#000000" },
    { SERENITY_ROLE_MENU_STRIPE,          "MenuStripe",               "#dbd8d1" },
    { SERENITY_ROLE_DISABLED_TEXT,        "DisabledTextFront",        "#808080" },
    { SERENITY_ROLE_DISABLED_TEXT_BACK,   "DisabledTextBack",         "#ffffff" },
    { SERENITY_ROLE_HOVER,                "HoverHighlight",           "#e3dfdb" },
    { SERENITY_ROLE_TITLE_ACTIVE_1,       "ActiveWindowBorder1",      "#09226e" },
    { SERENITY_ROLE_TITLE_ACTIVE_2,       "ActiveWindowBorder2",      "#9ecaf4" },
    { SERENITY_ROLE_TITLE_TEXT,           "ActiveWindowTitle",        "#ffffff" },
    { SERENITY_ROLE_TITLE_INACTIVE_1,     "InactiveWindowBorder1",    "#808080" },
    { SERENITY_ROLE_TITLE_INACTIVE_2,     "InactiveWindowBorder2",    "#c0c0c0" },
    { SERENITY_ROLE_TITLE_INACTIVE_TEXT,  "InactiveWindowTitle",      "#d5d0c7" },
    { SERENITY_ROLE_ACTIVE_BORDER,        "ActiveWindowBorder1",      "#09226e" },
    { SERENITY_ROLE_INACTIVE_BORDER,      "InactiveWindowBorder1",    "#808080" },
    { SERENITY_ROLE_GUTTER,               "Gutter",                   "#d4d0c8" },
    { SERENITY_ROLE_TOOLTIP,              "Tooltip",                  "#ffffe1" },
    { SERENITY_ROLE_TOOLTIP_TEXT,         "WindowText",               "#000000" },
    { SERENITY_ROLE_LINK,                 "Link",                     "#0000ff" },
    { SERENITY_ROLE_PLACEHOLDER,          "PlaceholderText",          "#808080" },
    { SERENITY_ROLE_FOCUS_OUTLINE,        "FocusOutline",             "#909090" },
};

static const int s_default_metrics[SERENITY_METRIC_COUNT] = {
    1,  /* BEVEL */
    1,  /* BORDER */
    0,  /* RADIUS — square corners, always */
    3,  /* FOCUS_MARGIN */
    23, /* BUTTON_MIN_H */
    16, /* SCROLLBAR_W */
    13, /* CHECK_SIZE */
    22, /* TITLEBAR_H */
};

/* ------------------------------------------------------------------ */
/* colour parsing                                                      */
/* ------------------------------------------------------------------ */

static const struct {
    const char *name;
    const char *hex;
} s_named[] = {
    { "white", "#ffffff" }, { "black", "#000000" }, { "red", "#ff0000" },
    { "green", "#00ff00" }, { "blue", "#0000ff" }, { "magenta", "#ff00ff" },
    { "cyan", "#00ffff" },  { "yellow", "#ffff00" }, { "orange", "#ffa500" },
    { "gray", "#808080" },  { "grey", "#808080" }, { "darkgray", "#404040" },
    { "lightgray", "#c0c0c0" },
};

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, SerenityColor *out)
{
    if (!s || s[0] != '#') return 0;
    const char *d = s + 1;
    size_t n = strlen(d);
    int v[8] = { 0 };
    size_t i;

    if (n == 3 || n == 4) {
        for (i = 0; i < n; i++) {
            int h = hex_nibble(d[i]);
            if (h < 0) return 0;
            v[i] = h * 17;
        }
    } else if (n == 6 || n == 8) {
        for (i = 0; i < n; i++) {
            int h = hex_nibble(d[i]);
            if (h < 0) return 0;
            v[i] = h;
        }
    } else {
        return 0;
    }

    if (n == 3 || n == 6) {
        out->r = (v[0] * 16 + v[1]) / 255.0;
        out->g = (v[2] * 16 + v[3]) / 255.0;
        out->b = (v[4] * 16 + v[5]) / 255.0;
        out->a = 1.0;
    } else if (n == 4) {
        out->r = v[0] / 255.0; out->g = v[1] / 255.0;
        out->b = v[2] / 255.0; out->a = v[3] / 255.0;
    } else {
        out->r = (v[0] * 16 + v[1]) / 255.0;
        out->g = (v[2] * 16 + v[3]) / 255.0;
        out->b = (v[4] * 16 + v[5]) / 255.0;
        out->a = (v[6] * 16 + v[7]) / 255.0;
    }
    return 1;
}

static const char *lookup_named(const char *s)
{
    size_t i;
    for (i = 0; i < sizeof(s_named) / sizeof(s_named[0]); i++)
        if (strcasecmp(s, s_named[i].name) == 0)
            return s_named[i].hex;
    return NULL;
}

static const char *raw_value(const char *key)
{
    int i;
    for (i = 0; i < S.color_count; i++)
        if (strcasecmp(S.colors[i].key, key) == 0)
            return S.colors[i].value;
    return NULL;
}

/* Resolve named colours, hex, "transparent" and key aliases such as
 * `Button=Window`. Depth-limited so a malformed theme cannot spin. */
static int resolve_value(const char *value, SerenityColor *out, int depth)
{
    const char *named;
    if (!value || depth > 8) return 0;
    if (strcasecmp(value, "transparent") == 0) {
        out->r = out->g = out->b = 0.0; out->a = 0.0;
        return 1;
    }
    if (parse_hex(value, out)) return 1;
    if ((named = lookup_named(value)) != NULL)
        return parse_hex(named, out);
    return resolve_value(raw_value(value), out, depth + 1);
}

/* ------------------------------------------------------------------ */
/* ini parsing                                                         */
/* ------------------------------------------------------------------ */

static void trim(char *s)
{
    char *p = s, *q;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    q = s + strlen(s);
    while (q > s && (isspace((unsigned char)q[-1]) || q[-1] == '\r')) *--q = '\0';
}

static void parse_theme_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    char section[64] = "";

    if (!f) return;
    S.color_count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        char *eq;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\r' || *p == '\n')
            continue;
        trim(p);
        if (p[0] == '[' && strchr(p, ']')) {
            char *end = strchr(p, ']');
            size_t len = (size_t)(end - p - 1);
            if (len >= sizeof(section)) len = sizeof(section) - 1;
            memcpy(section, p + 1, len);
            section[len] = '\0';
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        trim(p);
        trim(eq + 1);

        if (strcasecmp(section, "Colors") == 0) {
            if (S.color_count < MAX_ENTRIES) {
                snprintf(S.colors[S.color_count].key, KEY_LEN, "%.63s", p);
                snprintf(S.colors[S.color_count].value, VAL_LEN, "%.159s", eq + 1);
                S.color_count++;
            }
        } else if (strcasecmp(section, "Fonts") == 0) {
            if (strcasecmp(p, "Family") == 0)
                snprintf(S.font_family, sizeof(S.font_family), "%s", eq + 1);
            else if (strcasecmp(p, "Size") == 0)
                S.font_size = atoi(eq + 1);
        } else if (strcasecmp(section, "Metrics") == 0) {
            int i;
            for (i = 0; i < SERENITY_METRIC_COUNT; i++) {
                /* Metrics keys are the enum names without the prefix. */
                static const char *names[SERENITY_METRIC_COUNT] = {
                    "Bevel", "Border", "Radius", "FocusMargin",
                    "ButtonMinHeight", "ScrollbarWidth", "CheckSize", "TitlebarHeight",
                };
                if (strcasecmp(p, names[i]) == 0)
                    S.metrics[i] = atoi(eq + 1);
            }
        }
    }
    fclose(f);
}

static void read_config_name(const char *path, char *out, size_t len)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int in_theme = 0;
    snprintf(out, len, "Default");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *eq;
        while (*p && isspace((unsigned char)*p)) p++;
        trim(p);
        if (p[0] == '[') {
            in_theme = (strncasecmp(p, "[Theme]", 7) == 0);
            continue;
        }
        if (!in_theme) continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        trim(p); trim(eq + 1);
        if (strcasecmp(p, "Name") == 0) {
            snprintf(out, len, "%s", eq + 1);
            break;
        }
    }
    fclose(f);
}

static int file_exists(const char *p)
{
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void stamp_of(const char *p, struct timespec *out)
{
    struct stat st;
    out->tv_sec = 0; out->tv_nsec = 0;
    if (p && stat(p, &st) == 0) {
        out->tv_sec = st.st_mtim.tv_sec;
        out->tv_nsec = st.st_mtim.tv_nsec;
    }
}

static int stamps_equal(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static void find_config_path(void)
{
    const char *prefix = getenv("SERENITY_PREFIX");
    const char *candidates[6];
    char buf[512];
    int n = 0, i;

    if (prefix && prefix[0]) { snprintf(buf, sizeof(buf), "%s/etc/WindowServer.ini", prefix); candidates[n++] = buf; }
    candidates[n++] = "/serenity/etc/WindowServer.ini";
    candidates[n++] = "/etc/WindowServer.ini";
    for (i = 0; i < n; i++) {
        /* buf is only valid for candidates[0]; copy defensively. */
        if (file_exists(candidates[i])) {
            snprintf(S.config_path, sizeof(S.config_path), "%s", candidates[i]);
            return;
        }
    }
    S.config_path[0] = '\0';
}

static void find_theme_path(const char *name)
{
    const char *prefix = getenv("SERENITY_PREFIX");
    const char *theme_dir = getenv("SERENITY_THEME_DIR");
    const char *src = getenv("SERENITY_SOURCE_ROOT");
    char p[512];
    int i;
    const char *dirs[6];
    int n = 0;
    static char b0[512], b1[512], b2[512], b3[512];

    if (theme_dir && theme_dir[0]) { snprintf(b0, sizeof(b0), "%s", theme_dir); dirs[n++] = b0; }
    if (prefix && prefix[0]) { snprintf(b1, sizeof(b1), "%s/res/themes", prefix); dirs[n++] = b1; }
    dirs[n++] = "/res/themes";
    dirs[n++] = "/usr/share/serenity/themes";
    if (src && src[0]) { snprintf(b2, sizeof(b2), "%s/Base/res/themes", src); dirs[n++] = b2; }
    /* Development fallback: walk up from Ports/GTKSerenity/serenity-ui. */
    snprintf(b3, sizeof(b3), "%s/../../../Base/res/themes",
             getenv("SERENITY_BUILD_SOURCE_DIR")
                 ? getenv("SERENITY_BUILD_SOURCE_DIR")
                 : "/mnt/user/serenity-linuxgui/Ports/GTKSerenity/serenity-ui");
    dirs[n++] = b3;

    S.theme_path[0] = '\0';
    for (i = 0; i < n; i++) {
        snprintf(p, sizeof(p), "%s/%s.ini", dirs[i], name);
        if (file_exists(p)) { snprintf(S.theme_path, sizeof(S.theme_path), "%s", p); return; }
    }
    /* Menu names may contain '&' (e.g. "Redmond &2000"). */
    for (i = 0; i < n; i++) {
        snprintf(p, sizeof(p), "%s/Default.ini", dirs[i]);
        if (file_exists(p)) { snprintf(S.theme_path, sizeof(S.theme_path), "%s", p); return; }
    }
}

static void hex_of(SerenityColor c, char out[8])
{
    snprintf(out, 8, "#%02x%02x%02x",
             (unsigned)(c.r * 255.0 + 0.5), (unsigned)(c.g * 255.0 + 0.5),
             (unsigned)(c.b * 255.0 + 0.5));
}

/* Lock-free variant: build_css() already holds s_lock. Calling the public
 * accessor from there would deadlock on the non-recursive mutex. */
static SerenityColor color_unlocked(SerenityRole role)
{
    SerenityColor c = { 0, 0, 0, 1 };
    if (role < 0 || role >= SERENITY_ROLE_COUNT) return c;
    const char *v = raw_value(s_roles[role].key);
    if (!resolve_value(v ? v : s_roles[role].fallback, &c, 0))
        parse_hex(s_roles[role].fallback, &c);
    return c;
}

static char *build_css(int gtk_major)
{
    /* Large enough for the whole generated stylesheet. */
    size_t cap = 16384, len = 0;
    char *css = (char *)malloc(cap);
    char b[8];
    int i;
    if (!css) return NULL;

#define APPEND(...) do { \
        int _n = snprintf(css + len, cap - len, __VA_ARGS__); \
        if (_n > 0) len += (size_t)_n; \
        if (len >= cap - 512) { cap *= 2; char *_t = realloc(css, cap); if (!_t) { free(css); return NULL; } css = _t; } \
    } while (0)

    APPEND("/* Generated by libserenity-ui from the active SystemTheme. */\n");
    APPEND("/* Do not edit: switch with scripts/port/serenity-theme.sh.   */\n");
    {
        static const char *names[SERENITY_ROLE_COUNT] = {
            "face", "face_text", "window", "window_text", "base", "base_text",
            "highlight", "light", "shadow", "selection", "selection_text",
            "menu", "menu_text", "menu_selection", "menu_selection_text",
            "menu_stripe", "disabled_text", "disabled_text_back", "hover",
            "title_1", "title_2", "title_text", "title_inactive_1",
            "title_inactive_2", "title_inactive_text", "active_border",
            "inactive_border", "gutter", "tooltip", "tooltip_text", "link",
            "placeholder", "focus_outline",
        };
        for (i = 0; i < SERENITY_ROLE_COUNT; i++) {
            char h[8];
            hex_of(color_unlocked((SerenityRole)i), h);
            APPEND("@define-color serenity_%s %s;\n", names[i], h);
        }
    }
    hex_of(color_unlocked(SERENITY_ROLE_FACE), b);
    APPEND("\n/* Classic geometry: square corners, hard 1px edges, no shadows. */\n");
    APPEND("* { border-radius: 0; box-shadow: none; text-shadow: none; }\n");
    APPEND("window, .background, dialog, .dialog { background-color: %s; }\n", b);
    APPEND("* { font-family: %s; font-size: %dpt; }\n", S.font_family, S.font_size);

    /* libadwaita's named colours must resolve to the same palette, otherwise
     * GNOME apps keep their own whites/greys behind our repainted widgets. */
    APPEND("\n@define-color window_bg_color @serenity_face;\n");
    APPEND("@define-color window_fg_color @serenity_window_text;\n");
    APPEND("@define-color view_bg_color @serenity_base;\n");
    APPEND("@define-color view_fg_color @serenity_base_text;\n");
    APPEND("@define-color headerbar_bg_color @serenity_face;\n");
    APPEND("@define-color headerbar_fg_color @serenity_window_text;\n");
    APPEND("@define-color headerbar_border_color @serenity_shadow;\n");
    APPEND("@define-color headerbar_backdrop_color @serenity_face;\n");
    APPEND("@define-color card_bg_color @serenity_face;\n");
    APPEND("@define-color card_fg_color @serenity_window_text;\n");
    APPEND("@define-color dialog_bg_color @serenity_face;\n");
    APPEND("@define-color dialog_fg_color @serenity_window_text;\n");
    APPEND("@define-color popover_bg_color @serenity_menu;\n");
    APPEND("@define-color popover_fg_color @serenity_menu_text;\n");
    APPEND("@define-color accent_color @serenity_selection;\n");
    APPEND("@define-color accent_bg_color @serenity_selection;\n");
    APPEND("@define-color accent_fg_color @serenity_selection_text;\n");
    APPEND("@define-color destructive_color @serenity_shadow;\n");
    APPEND("@define-color sidebar_bg_color @serenity_gutter;\n");
    APPEND("@define-color shade_color alpha(@serenity_shadow, 0.2);\n");

    /* Global fallback for widgets we do not repaint in C (labels, icons,
     * custom-drawn app content). GTK4 dropped -GtkWidget-* properties. */
    APPEND("\nlabel, .dim-label, textview text, entry, treeview.view, list { ");
    APPEND("color: @serenity_window_text; background-color: transparent; }\n");
    APPEND("entry, textview, .view { background-color: @serenity_base; ");
    APPEND("color: @serenity_base_text; }\n");
    APPEND(":selected, :selected:focus { ");
    APPEND("background-color: @serenity_selection; color: @serenity_selection_text; }\n");
    APPEND("menu, popover, .menu { background-color: @serenity_menu; ");
    APPEND("background-image: none; color: @serenity_menu_text; ");
    APPEND("border: 1px solid @serenity_shadow; }\n");
    APPEND("tooltip, .tooltip { background-color: @serenity_tooltip; ");
    APPEND("color: @serenity_tooltip_text; }\n");
    /* background-image: none is what kills Adwaita's gradient on the bars;
     * without it a GTK3 header bar stays navy while its buttons turn beige. */
    APPEND("headerbar, headerbar.titlebar, .titlebar, .toolbar, toolbar, menubar { ");
    APPEND("background-color: @serenity_face; background-image: none; ");
    APPEND("color: @serenity_window_text; ");
    APPEND("border-bottom: 1px solid @serenity_shadow; box-shadow: none; }\n");
    APPEND("window.csd, window.csd > decoration, popover > contents { ");
    APPEND("box-shadow: none; border-radius: 0; margin: 0; }\n");
    APPEND("button, entry, .card, .boxed-list, listview > row, .sidebar { ");
    APPEND("border-radius: 0; box-shadow: none; transition: none; }\n");
    APPEND("*:disabled { color: @serenity_disabled_text; }\n");
    APPEND("link, .link { color: @serenity_link; }\n");
    if (gtk_major == 3) {
        APPEND("/* GTK3 keeps these legacy properties. */\n");
        APPEND("* { -GtkWidget-focus-line-width: 1; -GtkWidget-focus-padding: 2; }\n");
        APPEND(".scrollbar, scrollbar { -GtkRange-slider-width: %d; ",
               S.metrics[SERENITY_METRIC_SCROLLBAR_W]);
        APPEND("-GtkRange-trough-border: 0; -GtkScrollbar-has-backward-stepper: true; ");
        APPEND("-GtkScrollbar-has-forward-stepper: true; }\n");
    } else {
        /* GTK4: `contents` is a REAL widget (GtkPopoverContent, see
         * gtkpopovercontent.c), so gtk_widget_create_render_node() DOES visit
         * it and the GTK4 shim maps that CSS name to SERENITY_WIDGET_MENU:
         * LibGfx::ClassicStylePainter draws the whole menu panel there
         * (white base + black frame + 24px stripe).  Keep the stylesheet
         * background/border transparent so the stock CSS does not draw a
         * second rectangle on top of the painter's output.
         * (An earlier comment claimed this node was not a widget and blanked
         * it while nothing painted it -- that left GTK4 popover menus with no
         * panel at all, only floating text.) */
        APPEND("/* GTK4: the menu panel is painted by the shim, not by CSS. */\n");
        APPEND("popover > contents, popover > arrow { background-color: transparent; ");
        APPEND("background-image: none; border-style: none; box-shadow: none; }\n");
        /* GTK4 hides overlay scrollbars until hover by setting opacity: 0,
         * which stops them from being painted at all. The classic desktop
         * always shows a 16px bar. */
        APPEND("scrollbar { opacity: 1; }\n");
        APPEND("scrollbar.vertical { min-width: %dpx; }\n",
               S.metrics[SERENITY_METRIC_SCROLLBAR_W]);
        APPEND("scrollbar.horizontal { min-height: %dpx; }\n",
               S.metrics[SERENITY_METRIC_SCROLLBAR_W]);
    }
    APPEND("/* end of generated SerenityUI stylesheet */\n");
#undef APPEND
    return css;
}

static void do_load(void)
{
    char name[128];
    struct timespec cs, ts;
    int changed = 0;

    if (!S.config_path[0]) find_config_path();
    read_config_name(S.config_path, name, sizeof(name));
    find_theme_path(name);

    stamp_of(S.config_path, &cs);
    stamp_of(S.theme_path, &ts);
    if (!stamps_equal(&cs, &S.config_stamp) || !stamps_equal(&ts, &S.theme_stamp))
        changed = 1;
    if (strcmp(name, S.theme_name) != 0)
        changed = 1;

    if (!changed && S.initialized) return;

    snprintf(S.theme_name, sizeof(S.theme_name), "%s", name);
    S.config_stamp = cs;
    S.theme_stamp = ts;
    memcpy(S.metrics, s_default_metrics, sizeof(S.metrics));
    snprintf(S.font_family, sizeof(S.font_family), "Tahoma, DejaVu Sans, Sans-serif");
    S.font_size = 10;
    if (S.theme_path[0]) parse_theme_ini(S.theme_path);
    /* [Fonts] may override, but never below 6pt or above 48pt. */
    if (S.font_size < 6 || S.font_size > 48) S.font_size = 10;
    if (S.metrics[SERENITY_METRIC_BEVEL] < 1) S.metrics[SERENITY_METRIC_BEVEL] = 1;
    S.metrics[SERENITY_METRIC_RADIUS] = 0; /* hard requirement: square */

    free(S.css3); S.css3 = NULL;
    free(S.css4); S.css4 = NULL;
    S.css3 = build_css(3);
    S.css4 = build_css(4);
    S.revision++;
}

/* Caller must hold s_lock. Splitting this out is essential: ensure_fresh()
 * runs with the non-recursive mutex already held, so calling the public
 * serenity_ui_init() from there would deadlock on the very first access. */
static void init_unlocked(void)
{
    if (S.initialized) return;
    const char *legacy = getenv("SERENITY_GTK_LEGACY");
    S.active = !(legacy && strcmp(legacy, "1") == 0);
    S.revision = 0;
    S.font_family[0] = '\0';
    do_load();
    S.initialized = 1;
}

void serenity_ui_init(void)
{
    pthread_mutex_lock(&s_lock);
    init_unlocked();
    pthread_mutex_unlock(&s_lock);
}

static void ensure_fresh(void)
{
    if (!S.initialized) { init_unlocked(); return; }
    /* Cheap stat() poll; callers invoke this once per frame at most. */
    do_load();
}

const char *serenity_ui_theme_name(void)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    const char *r = S.theme_name;
    pthread_mutex_unlock(&s_lock);
    return r;
}

const char *serenity_ui_theme_path(void)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    const char *r = S.theme_path;
    pthread_mutex_unlock(&s_lock);
    return r;
}

SerenityColor serenity_ui_color(SerenityRole role)
{
    SerenityColor c;
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    c = color_unlocked(role);
    pthread_mutex_unlock(&s_lock);
    return c;
}

SerenityColor serenity_ui_color_by_name(const char *k)
{
    SerenityColor c = { 0, 0, 0, 1 };
    if (!k) return c;
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    const char *v = raw_value(k);
    if (!v || !resolve_value(v, &c, 0)) parse_hex("#000000", &c);
    pthread_mutex_unlock(&s_lock);
    return c;
}

const char *serenity_ui_color_hex(SerenityRole role, char buf[8])
{
    hex_of(serenity_ui_color(role), buf);
    return buf;
}

const char *serenity_ui_font_family(void)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    const char *r = S.font_family;
    pthread_mutex_unlock(&s_lock);
    return r;
}

int serenity_ui_font_size(void)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    int r = S.font_size;
    pthread_mutex_unlock(&s_lock);
    return r;
}

int serenity_ui_metric(SerenityMetric m)
{
    if (m < 0 || m >= SERENITY_METRIC_COUNT) return 0;
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    int r = S.metrics[m];
    pthread_mutex_unlock(&s_lock);
    return r;
}

uint64_t serenity_ui_revision(void)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    uint64_t r = S.revision;
    pthread_mutex_unlock(&s_lock);
    return r;
}

const char *serenity_ui_css(int gtk_major)
{
    pthread_mutex_lock(&s_lock);
    ensure_fresh();
    const char *r = gtk_major == 3 ? S.css3 : S.css4;
    pthread_mutex_unlock(&s_lock);
    return r;
}

int serenity_ui_is_active(void)
{
    serenity_ui_init();
    return S.active;
}

/* ---- widget kind vocabulary -------------------------------------------
 * Pure name mapping: no drawing lives in this library any more (see
 * serenity-paint-bridge). The GTK shims use this as their first lookup and
 * fall back to GType inheritance in gtkserenity.c.
 */
static const struct {
    const char *name;
    SerenityWidgetKind kind;
} s_kind_map[] = {
    { "GtkButton",              SERENITY_WIDGET_BUTTON },
    { "GtkToggleButton",        SERENITY_WIDGET_TOGGLE_BUTTON },
    { "GtkRadioButton",         SERENITY_WIDGET_TOGGLE_BUTTON },
    { "GtkCheckButton",         SERENITY_WIDGET_CHECK },
    { "GtkSwitch",              SERENITY_WIDGET_TOGGLE_BUTTON },
    { "GtkEntry",               SERENITY_WIDGET_ENTRY },
    { "GtkSpinButton",          SERENITY_WIDGET_ENTRY },
    { "GtkSearchEntry",         SERENITY_WIDGET_ENTRY },
    { "GtkPasswordEntry",       SERENITY_WIDGET_ENTRY },
    { "GtkTextView",            SERENITY_WIDGET_ENTRY },
    { "GtkComboBox",            SERENITY_WIDGET_COMBO },
    { "GtkComboBoxText",        SERENITY_WIDGET_COMBO },
    { "GtkDropDown",            SERENITY_WIDGET_COMBO },
    { "GtkFrame",               SERENITY_WIDGET_FRAME_RAISED },
    { "GtkAspectFrame",         SERENITY_WIDGET_FRAME_RAISED },
    { "GtkScrollbar",           SERENITY_WIDGET_SCROLLBAR_TROUGH },
    { "GtkScale",               SERENITY_WIDGET_SCROLLBAR_TROUGH },
    { "GtkProgressBar",         SERENITY_WIDGET_PROGRESS_TROUGH },
    { "GtkHeaderBar",           SERENITY_WIDGET_HEADER_BAR },
    { "AdwHeaderBar",           SERENITY_WIDGET_HEADER_BAR },
    { "GtkToolbar",             SERENITY_WIDGET_TOOLBAR },
    { "GtkMenuBar",             SERENITY_WIDGET_MENU_BAR },
    { "GtkMenu",                SERENITY_WIDGET_MENU },
    { "GtkMenuItem",            SERENITY_WIDGET_MENU_ITEM },
    { "GtkPopoverMenu",         SERENITY_WIDGET_MENU },
    { "GtkPopover",             SERENITY_WIDGET_MENU },
    { "GtkStatusbar",           SERENITY_WIDGET_STATUS_BAR },
    { "GtkNotebook",            SERENITY_WIDGET_TAB },
    { "GtkStackSwitcher",       SERENITY_WIDGET_TAB },
    { "GtkTreeView",            SERENITY_WIDGET_ENTRY },
    { "GtkIconView",            SERENITY_WIDGET_ENTRY },
    { "GtkColumnView",          SERENITY_WIDGET_ENTRY },
    { "GtkListBox",             SERENITY_WIDGET_ENTRY },
    { "GtkSeparator",           SERENITY_WIDGET_SEPARATOR },
    { "GtkPaned",               SERENITY_WIDGET_HANDLE },
    { "GtkDialog",              SERENITY_WIDGET_FRAME_RAISED },
    { "GtkWindow",              SERENITY_WIDGET_NONE },
    { "GtkApplicationWindow",   SERENITY_WIDGET_NONE },
    { "AdwWindow",              SERENITY_WIDGET_NONE },
    { "AdwApplicationWindow",   SERENITY_WIDGET_NONE },
};

SerenityWidgetKind serenity_widget_kind_from_name(const char *name)
{
    size_t i;
    if (!name) return SERENITY_WIDGET_NONE;
    for (i = 0; i < sizeof(s_kind_map) / sizeof(s_kind_map[0]); i++)
        if (strcmp(name, s_kind_map[i].name) == 0)
            return s_kind_map[i].kind;
    return SERENITY_WIDGET_NONE;
}

const char *serenity_widget_kind_name(SerenityWidgetKind kind)
{
    static const char *names[SERENITY_WIDGET_COUNT] = {
        "button", "toggle-button", "entry", "frame-raised", "frame-sunken",
        "group-box", "scrollbar-trough", "scrollbar-thumb", "scrollbar-button",
        "check", "radio", "menu-bar", "menu", "menu-item", "toolbar",
        "header-bar", "tab", "tab-active", "progress-trough", "progress-fill",
        "status-bar", "list-header", "combo", "handle", "separator",
        "window-titlebar",
    };
    if (kind < 0 || kind >= SERENITY_WIDGET_COUNT) return "none";
    return names[kind];
}
