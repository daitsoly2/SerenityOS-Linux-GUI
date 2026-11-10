/* serenity-ui.h — the single source of truth for the Serenity desktop look.
 *
 * This is deliberately a plain C library with no GLib/GObject/GTK dependency.
 * GTK3, GTK4 and libadwaita all link against it, so a GTK app and a native
 * LibGUI app read *the same* SystemTheme colours. No gtk.css, no GTK_THEME,
 * no environment variables are needed by applications.
 *
 * Theme source (in order):
 *   $SERENITY_PREFIX/etc/WindowServer.ini  -> [Theme] Name=
 *   $SERENITY_PREFIX/res/themes/<Name>.ini -> [Colors] / [Fonts] / [Metrics]
 * with fallbacks to /etc, /res and the source tree for development.
 */
#ifndef SERENITY_UI_H
#define SERENITY_UI_H

#include <stdint.h>

/* SERENITY_HOOK: every symbol introduced by this fork is hidden.
   The exported symbol tables of libgtk-3.so.0 / libgtk-4.so.1 /
   libadwaita-1.so.0 stay byte-for-byte identical to upstream. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double r, g, b, a; /* 0.0 .. 1.0 */
} SerenityColor;

/* Semantic roles, mapped onto the keys of a SerenityOS theme ini file.
 * These are the same values Gfx::Palette hands to LibGUI widgets, so a
 * GTK button and a GUI::Button resolve to exactly the same colour. */
typedef enum {
    SERENITY_ROLE_FACE = 0,           /* Button / Window */
    SERENITY_ROLE_FACE_TEXT,          /* ButtonText */
    SERENITY_ROLE_WINDOW,             /* Window */
    SERENITY_ROLE_WINDOW_TEXT,        /* WindowText */
    SERENITY_ROLE_BASE,               /* Base (entry, list, text view) */
    SERENITY_ROLE_BASE_TEXT,
    SERENITY_ROLE_HIGHLIGHT,          /* ThreedHighlight (outer light bevel) */
    SERENITY_ROLE_LIGHT,              /* ThreedShadow1 (inner light bevel) */
    SERENITY_ROLE_SHADOW,             /* ThreedShadow2 (dark bevel) */
    SERENITY_ROLE_SELECTION,
    SERENITY_ROLE_SELECTION_TEXT,
    SERENITY_ROLE_MENU,
    SERENITY_ROLE_MENU_TEXT,
    SERENITY_ROLE_MENU_SELECTION,
    SERENITY_ROLE_MENU_SELECTION_TEXT,
    SERENITY_ROLE_MENU_STRIPE,
    SERENITY_ROLE_DISABLED_TEXT,
    SERENITY_ROLE_DISABLED_TEXT_BACK,
    SERENITY_ROLE_HOVER,
    SERENITY_ROLE_TITLE_ACTIVE_1,     /* gradient start */
    SERENITY_ROLE_TITLE_ACTIVE_2,     /* gradient end */
    SERENITY_ROLE_TITLE_TEXT,
    SERENITY_ROLE_TITLE_INACTIVE_1,
    SERENITY_ROLE_TITLE_INACTIVE_2,
    SERENITY_ROLE_TITLE_INACTIVE_TEXT,
    SERENITY_ROLE_ACTIVE_BORDER,
    SERENITY_ROLE_INACTIVE_BORDER,
    SERENITY_ROLE_GUTTER,
    SERENITY_ROLE_TOOLTIP,
    SERENITY_ROLE_TOOLTIP_TEXT,
    SERENITY_ROLE_LINK,
    SERENITY_ROLE_PLACEHOLDER,
    SERENITY_ROLE_FOCUS_OUTLINE,
    SERENITY_ROLE_COUNT
} SerenityRole;

/* Geometry of the classic Serenity widget set. All values are pixels. */
typedef enum {
    SERENITY_METRIC_BEVEL = 0,       /* width of one 3D bevel edge      (1) */
    SERENITY_METRIC_BORDER,          /* frame border                    (1) */
    SERENITY_METRIC_RADIUS,          /* corner radius — always 0        (0) */
    SERENITY_METRIC_FOCUS_MARGIN,    /* dotted focus rect inset         (3) */
    SERENITY_METRIC_BUTTON_MIN_H,    /* classic button height          (23) */
    SERENITY_METRIC_SCROLLBAR_W,     /* classic scrollbar width        (16) */
    SERENITY_METRIC_CHECK_SIZE,      /* check/radio box                 (13) */
    SERENITY_METRIC_TITLEBAR_H,      /* title bar height                (22) */
    SERENITY_METRIC_COUNT
} SerenityMetric;

/* ---- the widget vocabulary -------------------------------------------
 * These enums are the ONLY thing the GTK forks use to describe a widget.
 * Drawing is not implemented here: it is forwarded to Serenity's real
 * Gfx::StylePainter through libserenitypaint.so (see serenity-paint-bridge),
 * so there is exactly one implementation of the classic look in the project.
 */
typedef enum {
    SERENITY_WIDGET_NONE = -1,
    SERENITY_WIDGET_BUTTON = 0,
    SERENITY_WIDGET_TOGGLE_BUTTON,
    SERENITY_WIDGET_ENTRY,
    SERENITY_WIDGET_FRAME_RAISED,
    SERENITY_WIDGET_FRAME_SUNKEN,
    SERENITY_WIDGET_GROUP_BOX,
    SERENITY_WIDGET_SCROLLBAR_TROUGH,
    SERENITY_WIDGET_SCROLLBAR_THUMB,
    SERENITY_WIDGET_SCROLLBAR_BUTTON,
    SERENITY_WIDGET_CHECK,
    SERENITY_WIDGET_RADIO,
    SERENITY_WIDGET_MENU_BAR,
    SERENITY_WIDGET_MENU,
    SERENITY_WIDGET_MENU_ITEM,
    SERENITY_WIDGET_MENU_SELECTION,
    SERENITY_WIDGET_TOOLBAR,
    SERENITY_WIDGET_HEADER_BAR,
    SERENITY_WIDGET_TAB,
    SERENITY_WIDGET_TAB_ACTIVE,
    SERENITY_WIDGET_PROGRESS_TROUGH,
    SERENITY_WIDGET_PROGRESS_FILL,
    SERENITY_WIDGET_STATUS_BAR,
    SERENITY_WIDGET_LIST_HEADER,
    SERENITY_WIDGET_COMBO,
    SERENITY_WIDGET_HANDLE,
    SERENITY_WIDGET_SEPARATOR,
    SERENITY_WIDGET_WINDOW_TITLEBAR,
    SERENITY_WIDGET_COUNT
} SerenityWidgetKind;

typedef enum {
    SERENITY_STATE_NORMAL   = 0,
    SERENITY_STATE_HOVER    = 1 << 0,
    SERENITY_STATE_PRESSED  = 1 << 1,
    SERENITY_STATE_CHECKED  = 1 << 2,
    SERENITY_STATE_DISABLED = 1 << 3,
    SERENITY_STATE_FOCUSED  = 1 << 4,
    SERENITY_STATE_DEFAULT  = 1 << 5,
    SERENITY_STATE_SELECTED = 1 << 6,
    SERENITY_STATE_BACKDROP = 1 << 7
} SerenityWidgetState;

typedef enum {
    SERENITY_ARROW_NONE = 0,
    SERENITY_ARROW_UP,
    SERENITY_ARROW_DOWN,
    SERENITY_ARROW_LEFT,
    SERENITY_ARROW_RIGHT
} SerenityArrow;

typedef struct {
    SerenityArrow arrow;
    double        value;     /* 0..1 for progress / slider */
    int           vertical;  /* scrollbar / progress orientation */
    int           max_value; /* progress: upper bound, 0 means 100 */
    const char   *text;      /* titlebar caption, group box label */
} SerenityWidgetExtra;

/* GTK type/class name -> widget kind (exact match; the shims add GType
 * inheritance on top). Returns SERENITY_WIDGET_NONE when the widget must
 * keep its stock rendering. */
SerenityWidgetKind serenity_widget_kind_from_name (const char *name);
const char       *serenity_widget_kind_name (SerenityWidgetKind kind);

/* Idempotent. Called automatically by every accessor. */
void              serenity_ui_init (void);

const char       *serenity_ui_theme_name (void);
const char       *serenity_ui_theme_path (void);

SerenityColor     serenity_ui_color (SerenityRole role);
/* Look up any raw [Colors] key, e.g. "ActiveWindowBorder2". */
SerenityColor     serenity_ui_color_by_name (const char *key);
/* "#rrggbb" into a caller buffer of at least 8 bytes. */
const char       *serenity_ui_color_hex (SerenityRole role, char buf[8]);

const char       *serenity_ui_font_family (void);
int               serenity_ui_font_size (void);
int               serenity_ui_metric (SerenityMetric metric);

/* Increments whenever WindowServer.ini or the theme file changes. GTK uses
 * this to reload its style provider, so `serenity-theme.sh vista|classic`
 * updates running GTK applications without restarting the session. */
uint64_t          serenity_ui_revision (void);

/* Base CSS (named colours + global square/no-shadow/font rules) generated
 * from the current theme. Optionally GTK3-specific (gtk_major == 3).
 * The returned pointer belongs to the library and stays valid until the
 * next reload; never free it. */
const char       *serenity_ui_css (int gtk_major);

/* Escape hatch for A/B testing: set SERENITY_GTK_LEGACY=1 to make the
 * patched GTK fall back to its stock Adwaita rendering. */
int               serenity_ui_is_active (void);

#ifdef __cplusplus
}
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
#endif /* SERENITY_UI_H */
