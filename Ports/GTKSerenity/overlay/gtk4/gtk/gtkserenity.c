/* GTK4 -> Serenity widget rendering bridge.
 *
 * GTK4 has one shared widget-painting chokepoint: gtk_widget_create_render_node()
 * paints the CSS background/border at the widget's border box. We intercept it
 * with _gtk_serenity_paint(): Serenity's REAL Gfx::StylePainter draws the
 * widget into a bitmap (via libserenitypaint.so, dlopen'd, so libgtk stays
 * pure C) and we blit those pixels into the render node tree.
 *
 * There is no cairo reimplementation of the classic look any more: what a GTK
 * widget shows is literally LibGfx/ClassicStylePainter output, the same code
 * that paints a native GUI::Button on the Serenity desktop.
 *
 * Which widget is which comes from three sources, in order:
 *   1. the CSS node name ("trough", "slider", "tab", "progress", ...), which
 *      is what GTK's internal GtkGizmo children are known by;
 *   2. the exact GType name;
 *   3. GType inheritance, so GtkMenuButton / GtkSourceView / AdwSplitButton's
 *      inner GtkButton inherit their ancestor's look.
 *
 * libadwaita widgets are GTK4 widgets, so they inherit this automatically.
 */
#include "config.h"
#include "gtkserenity.h"

#include "gtksnapshotprivate.h"
#include "gtkcssprovider.h"
#include "gtkstyleprovider.h"
#include "gtkwidgetprivate.h"

#include "gtkadjustment.h"
#include "gtkorientable.h"
#include "gtkprogressbar.h"
#include "gtkscale.h"
#include "gtkscrollbar.h"

#include "serenity-ui.h"

#include <cairo.h>
#include <dlfcn.h>
#include <graphene.h>
#include <math.h>
#include <string.h>

/* SERENITY_HOOK: forward widget painting to Serenity's REAL painter via
 * libserenitypaint.so (C ABI, links the Lagom host build of LibGfx). */
typedef int (*serenity_paint_widget_fn)(int, int, unsigned char *, int, int,
                                        int, double, int, int);
typedef int (*serenity_paint_scrollbar_fn)(int, int, unsigned char *, int,
                                           int, int, int, int);
static serenity_paint_widget_fn s_paint_widget;
static serenity_paint_scrollbar_fn s_paint_scrollbar;
static gboolean s_bridge_tried;

static void load_bridge(void)
{
    const char *prefix;
    char path[512];
    void *h = NULL;

    if (s_bridge_tried) return;
    s_bridge_tried = TRUE;

    prefix = g_getenv("SERENITY_PREFIX");
    if (prefix && *prefix) {
        snprintf(path, sizeof path, "%s/lib/libserenitypaint.so", prefix);
        h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    }
    if (!h)
        h = dlopen("libserenitypaint.so", RTLD_LAZY | RTLD_LOCAL);
    if (h) {
        s_paint_widget = (serenity_paint_widget_fn) dlsym(h, "serenity_paint_widget");
        s_paint_scrollbar = (serenity_paint_scrollbar_fn) dlsym(h, "serenity_paint_scrollbar");
    }
}

#define MAX_DEPTH 64

static __thread SerenityWidgetKind s_kind_stack[MAX_DEPTH];
static __thread gboolean           s_suppress_stack[MAX_DEPTH];
static __thread int               s_depth;

static GtkCssProvider *s_provider;
static guint64         s_css_revision;
static gboolean        s_inited;

static gboolean reload_theme(gpointer data)
{
    guint64 rev;
    (void)data;
    if (!s_provider) return G_SOURCE_CONTINUE;
    rev = serenity_ui_revision();
    if (rev != s_css_revision) {
        gtk_css_provider_load_from_data(s_provider, serenity_ui_css(4), -1);
        s_css_revision = rev;
    }
    return G_SOURCE_CONTINUE;
}

/* The system-wide theme lives in /usr/share/themes/Serenity (GTK_THEME).
 * GTK loads it at THEME priority (200), but libadwaita ships its own
 * stylesheet at APPLICATION (600) and would override the typography and
 * menu-row metrics defined there.  Load the very same file again through the
 * shim at a higher priority so there is ONE global source of truth for every
 * GTK4 / libadwaita process -- no per-user ~/.config/gtk-4.0/gtk.css needed.
 * The generated palette stylesheet (serenity_ui_css) stays separate: it is
 * rebuilt from WindowServer.ini and can hot-reload on a theme change. */
static void install_global_theme(GdkDisplay *display)
{
    static const char *paths[] = {
        "/usr/share/themes/Serenity/gtk-4.0/gtk.css",
        NULL,
    };
    char prefixed[512];
    const char *prefix = g_getenv("SERENITY_PREFIX");
    GtkCssProvider *provider;
    const char *path = NULL;
    int i;

    if (prefix && *prefix) {
        snprintf(prefixed, sizeof prefixed,
                 "%s/share/themes/Serenity/gtk-4.0/gtk.css", prefix);
        if (g_file_test(prefixed, G_FILE_TEST_EXISTS))
            path = prefixed;
    }
    if (!path) {
        for (i = 0; paths[i]; i++) {
            if (g_file_test(paths[i], G_FILE_TEST_EXISTS)) {
                path = paths[i];
                break;
            }
        }
    }
    if (!path)
        return;

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, path);
    gtk_style_context_add_provider_for_display(display,
                                                GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    /* The display keeps a ref on the provider; drop ours. */
    g_object_unref(provider);
}

void _gtk_serenity_init(GdkDisplay *display)
{
    if (s_inited || !display) return;
    s_inited = TRUE;
    /* SERENITY_GTK_LEGACY=1 must give byte-for-byte stock Adwaita: our
     * stylesheet also blanks the CSS backgrounds that the painter replaces,
     * so it must not be installed when painting is disabled. */
    if (!serenity_ui_is_active()) return;
    s_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(s_provider, serenity_ui_css(4), -1);
    s_css_revision = serenity_ui_revision();
    gtk_style_context_add_provider_for_display(display,
                                                GTK_STYLE_PROVIDER(s_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    install_global_theme(display);
    g_timeout_add(1000, reload_theme, NULL);
}

static SerenityWidgetState state_of(GtkWidget *widget)
{
    GtkStateFlags f = gtk_widget_get_state_flags(widget);
    SerenityWidgetState s = SERENITY_STATE_NORMAL;
    if (f & GTK_STATE_FLAG_ACTIVE)      s |= SERENITY_STATE_PRESSED;
    if (f & GTK_STATE_FLAG_PRELIGHT)    s |= SERENITY_STATE_HOVER;
    if (f & GTK_STATE_FLAG_SELECTED)    s |= SERENITY_STATE_SELECTED;
    if (f & GTK_STATE_FLAG_CHECKED)     s |= SERENITY_STATE_CHECKED;
    if (f & GTK_STATE_FLAG_INSENSITIVE) s |= SERENITY_STATE_DISABLED;
    if (f & GTK_STATE_FLAG_FOCUSED)     s |= SERENITY_STATE_FOCUSED;
    if (f & GTK_STATE_FLAG_BACKDROP)    s |= SERENITY_STATE_BACKDROP;
    return s;
}

/* Real apps use subclasses (GtkMenuButton, GtkSourceView, GtkDropDown, ...).
 * g_type_from_name() keeps this here without pulling in every gtk header. */
static SerenityWidgetKind kind_from_gtype(GType t)
{
    static const struct { const char *name; SerenityWidgetKind kind; } m[] = {
        { "GtkModelButton",       SERENITY_WIDGET_MENU_ITEM },
        { "GtkToggleButton",      SERENITY_WIDGET_TOGGLE_BUTTON },
        { "GtkSwitch",            SERENITY_WIDGET_TOGGLE_BUTTON },
        { "GtkMenuButton",        SERENITY_WIDGET_BUTTON },
        { "GtkScaleButton",       SERENITY_WIDGET_BUTTON },
        { "GtkColorButton",       SERENITY_WIDGET_BUTTON },
        { "GtkFontButton",        SERENITY_WIDGET_BUTTON },
        { "GtkFileChooserButton", SERENITY_WIDGET_BUTTON },
        { "GtkButton",            SERENITY_WIDGET_BUTTON },
        { "GtkSpinButton",        SERENITY_WIDGET_ENTRY },
        { "GtkEntry",             SERENITY_WIDGET_ENTRY },
        { "GtkTextView",          SERENITY_WIDGET_ENTRY },
        { "GtkDropDown",          SERENITY_WIDGET_COMBO },
        { "GtkProgressBar",       SERENITY_WIDGET_PROGRESS_TROUGH },
        { "GtkHeaderBar",         SERENITY_WIDGET_HEADER_BAR },
        { "GtkStackSwitcher",     SERENITY_WIDGET_TAB },
        { "GtkSeparator",         SERENITY_WIDGET_SEPARATOR },
        { "GtkPaned",             SERENITY_WIDGET_HANDLE },
        { "GtkMenuBar",           SERENITY_WIDGET_MENU_BAR },
        { "GtkPopoverMenu",       SERENITY_WIDGET_MENU },
        { "GtkPopover",           SERENITY_WIDGET_MENU },
        { "GtkListBox",           SERENITY_WIDGET_ENTRY },
        { "GtkColumnView",        SERENITY_WIDGET_ENTRY },
        { "GtkTreeView",          SERENITY_WIDGET_ENTRY },
        { "GtkStatusBar",         SERENITY_WIDGET_STATUS_BAR },
    };
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(m); i++) {
        GType b = g_type_from_name(m[i].name);
        if (b != G_TYPE_INVALID && g_type_is_a(t, b))
            return m[i].kind;
    }
    return SERENITY_WIDGET_NONE;
}

/* GTK's own internal children are GtkGizmo widgets known only by their CSS
 * node name. This is where a notebook tab, a range trough, a scrollbar slider
 * and a progress bar's filled part live, so it is also where the exact
 * geometry for them comes from. */
static SerenityWidgetKind kind_from_css_name(GtkWidget *widget)
{
    const char *name = gtk_widget_get_css_name(widget);
    if (!name) return SERENITY_WIDGET_NONE;

    if (strcmp(name, "tab") == 0)
        return SERENITY_WIDGET_TAB;

    if (strcmp(name, "slider") == 0)
        return SERENITY_WIDGET_SCROLLBAR_THUMB;

    if (strcmp(name, "trough") == 0) {
        GtkWidget *parent = gtk_widget_get_parent(widget);
        if (parent && GTK_IS_PROGRESS_BAR(parent))
            return SERENITY_WIDGET_PROGRESS_TROUGH;
        if (parent && GTK_IS_SCALE(parent))
            return SERENITY_WIDGET_FRAME_SUNKEN;
        if (parent && GTK_IS_SCROLLBAR(parent))
            return SERENITY_WIDGET_SCROLLBAR_TROUGH;
    }

    /* GtkCheckButton is the whole row; the box itself is the indicator child
     * (a GtkBuiltinIcon), so the caller resolves its geometry. */
    if (strcmp(name, "checkbutton") == 0)
        return SERENITY_WIDGET_CHECK;

    if (strcmp(name, "menuitem") == 0)
        return SERENITY_WIDGET_MENU_ITEM;

    /* The visible menu panel is a REAL widget: GtkPopoverContent, whose CSS
     * name is "contents" (gtkpopovercontent.c:78).  An earlier version of
     * this file assumed it was a bare CSS node that gtk_widget_create_render_
     * node() never visits, blanked its stylesheet background, and left the
     * whole popover with no panel at all -- only the menu text floated over
     * the window.  It IS a widget, so the hook runs: paint the complete
     * Serenity menu here (white base + black frame + 24px stripe) and keep
     * `popover > contents` transparent in CSS so it is not drawn twice.
     * "contents" is unique to GtkPopoverContent in GTK4. */
    if (strcmp(name, "contents") == 0)
        return SERENITY_WIDGET_MENU;

    return SERENITY_WIDGET_NONE;
}

/* Nodes that GTK paints itself but whose box the Serenity painter already
 * covered. They must not draw a stock CSS background on top of it. */
static gboolean is_covered_node(GtkWidget *widget)
{
    const char *name = gtk_widget_get_css_name(widget);
    if (!name) return FALSE;
    /* the check/radio mark glyph and the progress bar's filled part */
    if (strcmp(name, "check") == 0 || strcmp(name, "radio") == 0)
        return TRUE;
    if (strcmp(name, "progress") == 0)
        return TRUE;
    return FALSE;
}

static SerenityWidgetKind kind_of_widget(GtkWidget *widget)
{
    SerenityWidgetKind kind = kind_from_css_name(widget);
    if (kind != SERENITY_WIDGET_NONE)
        return kind;
    kind = serenity_widget_kind_from_name(G_OBJECT_TYPE_NAME(widget));
    if (kind != SERENITY_WIDGET_NONE)
        return kind;
    return kind_from_gtype(G_OBJECT_TYPE(widget));
}

/* Some widgets are only a frame around a real GtkButton (GtkMenuButton,
 * GtkDropDown, GtkSplitButton, GtkScaleButton, ...). Their border box is
 * larger than the face the user sees, so painting a bevel there leaves a
 * stray 1px "⌐" in the corner. Suppress the wrapper: the inner button is a
 * widget too and paints its own bevel at exactly its own geometry. */
static gboolean face_is_a_child_button(GtkWidget *widget, SerenityWidgetKind kind)
{
    GtkWidget *child;
    SerenityWidgetKind child_kind;

    if (kind != SERENITY_WIDGET_BUTTON &&
        kind != SERENITY_WIDGET_TOGGLE_BUTTON &&
        kind != SERENITY_WIDGET_COMBO)
        return FALSE;

    child = gtk_widget_get_first_child(widget);
    if (!child)
        return FALSE;
    child_kind = kind_of_widget(child);
    return child_kind == SERENITY_WIDGET_BUTTON ||
           child_kind == SERENITY_WIDGET_TOGGLE_BUTTON ||
           child_kind == SERENITY_WIDGET_COMBO;
}

/* GtkPopover / GtkPopoverMenu also map to MENU by GType, but the panel is
 * painted by their "contents" child (GtkPopoverContent).  Drawing both would
 * stack two black frames and two stripes, and the outer one has the wrong
 * bounds (it includes the arrow / shadow padding).  Suppress the wrapper. */
static gboolean panel_is_a_child_contents(GtkWidget *widget, SerenityWidgetKind kind)
{
    const char *name;
    GtkWidget *child;

    if (kind != SERENITY_WIDGET_MENU)
        return FALSE;
    name = gtk_widget_get_css_name(widget);
    if (name && strcmp(name, "contents") == 0)
        return FALSE; /* this one IS the panel */
    for (child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        const char *child_name = gtk_widget_get_css_name(child);
        if (child_name && strcmp(child_name, "contents") == 0)
            return TRUE;
    }
    return FALSE;
}

void _gtk_serenity_snapshot_begin(GtkWidget *widget, GtkSnapshot *snapshot)
{
    (void)widget; (void)snapshot;
    if (s_depth < MAX_DEPTH)
        s_depth++;
}

void _gtk_serenity_snapshot_end(GtkWidget *widget)
{
    (void)widget;
    if (s_depth > 0) s_depth--;
}

/* Forward GTK's per-widget "paint the background/border" request to the
 * Serenity painter, at the exact border-box geometry GTK would use for the
 * stock CSS background. This runs from gtk_widget_create_render_node(), the
 * one path every widget (root and nested children alike) goes through.
 * Returns TRUE when the pixels were produced and stock CSS must be skipped. */
gboolean _gtk_serenity_paint(GtkWidget *widget, GtkSnapshot *snapshot,
                             const graphene_rect_t *box)
{
    SerenityWidgetKind kind;
    SerenityWidgetExtra extra = { SERENITY_ARROW_NONE, 0.0, 0, 0, NULL };
    SerenityWidgetState state;
    graphene_rect_t r;
    cairo_t *cr;
    cairo_surface_t *surf;
    unsigned char *px;
    int w, h, rc;

    if (!widget || !snapshot || !box) return FALSE;
    if (box->size.width <= 0 || box->size.height <= 0) return FALSE;

    _gtk_serenity_init(gtk_widget_get_display(widget));
    if (!serenity_ui_is_active()) return FALSE;

    /* state_of() was declared but never called: `state` used to be an
     * uninitialised stack value, so hover / pressed / checked / disabled were
     * effectively random for every GTK4 widget (and the compiler warned about
     * state_of being unused).  Read the real GTK state flags before any paint
     * decision is made. */
    state = state_of(widget);
    kind = kind_of_widget(widget);

    /* Bars (header/tool/menu/status) stay FLAT — only actual controls get
     * bevels. Painting a bevel across a whole bar draws lines that cross the
     * child buttons and leaves stray corner ticks; the Serenity-tinted CSS
     * background already covers them. */
    if (kind == SERENITY_WIDGET_HEADER_BAR ||
        kind == SERENITY_WIDGET_TOOLBAR ||
        kind == SERENITY_WIDGET_MENU_BAR ||
        kind == SERENITY_WIDGET_STATUS_BAR)
        return FALSE;

    if (is_covered_node(widget))
        return TRUE; /* suppress the stock box, the painter already has it */

    if (kind == SERENITY_WIDGET_NONE)
        return FALSE;

    if (face_is_a_child_button(widget, kind))
        return TRUE; /* suppress the wrapper's own background entirely */

    if (panel_is_a_child_contents(widget, kind))
        return TRUE; /* the "contents" child paints the real menu panel */

    /* Take the SIZE from the CSS border box (that is the face the user sees;
     * gtk_widget_get_width() reports the smaller content box) but paint at the
     * widget's own origin: the box origin is in the parent's coordinate space
     * for nested widgets, and using it shifted the artwork so that only one
     * corner of the bevel stayed on screen. */
    w = (int) round(box->size.width);
    h = (int) round(box->size.height);
    if (w < 1) w = gtk_widget_get_width(widget);
    if (h < 1) h = gtk_widget_get_height(widget);
    r.origin.x = 0;
    r.origin.y = 0;
    r.size.width = w;
    r.size.height = h;

    /* A check button's row is as wide as the window; the box to draw is the
     * indicator's. The tick glyph itself stays GTK's, drawn over our box. */
    if (kind == SERENITY_WIDGET_CHECK || kind == SERENITY_WIDGET_RADIO) {
        GtkWidget *indicator = gtk_widget_get_first_child(widget);
        graphene_rect_t ib;
        if (gtk_widget_has_css_class(widget, "radio"))
            kind = SERENITY_WIDGET_RADIO;
        if (indicator && gtk_widget_compute_bounds(indicator, widget, &ib)) {
            r.origin.x = round(ib.origin.x);
            r.origin.y = round(ib.origin.y);
            r.size.width = round(ib.size.width);
            r.size.height = round(ib.size.height);
            w = (int) r.size.width;
            h = (int) r.size.height;
            state &= ~SERENITY_STATE_CHECKED;
        }
    }
    if (w < 1 || h < 1) return FALSE;

    /* The progress bar paints itself: one paint_progressbar() call covers the
     * gradient bar and the carved-out remainder. */
    if (kind == SERENITY_WIDGET_PROGRESS_TROUGH) {
        GtkWidget *bar = GTK_IS_PROGRESS_BAR(widget) ? widget : gtk_widget_get_parent(widget);
        if (bar && GTK_IS_PROGRESS_BAR(bar)) {
            extra.value = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(bar));
            extra.vertical = gtk_orientable_get_orientation(GTK_ORIENTABLE(bar))
                             == GTK_ORIENTATION_VERTICAL;
        }
        extra.max_value = 100;
    }

    if (g_getenv("SERENITY_DEBUG_KIND"))
        g_printerr("SERENITY_PAINT %s(%s) kind=%d %dx%d\n",
                   G_OBJECT_TYPE_NAME(widget), gtk_widget_get_css_name(widget),
                   (int) kind, w, h);

    load_bridge();
    if (!s_paint_widget)
        return FALSE;

    /* The pixels must live in a cairo-owned surface: GTK records the drawing
     * into a display list and replays it at render time, long after this
     * function returns. Handing cairo a caller buffer (as the first version
     * did) let the buffer be freed before the replay, so the widget came out
     * transparent. */
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return FALSE;
    }
    px = cairo_image_surface_get_data(surf);
    memset(px, 0, (size_t) cairo_image_surface_get_stride(surf) * (size_t) h);
    /* A GtkScrollbar is one widget: GTK's own trough/slider are gadgets, not
     * widgets, so the whole classic bar (dithered gutter + scrubber) is drawn
     * here from the adjustment's geometry. */
    if (kind == SERENITY_WIDGET_SCROLLBAR_TROUGH && GTK_IS_SCROLLBAR(widget)) {
        GtkAdjustment *adj = gtk_scrollbar_get_adjustment(GTK_SCROLLBAR(widget));
        int vertical = gtk_orientable_get_orientation(GTK_ORIENTABLE(widget))
                       == GTK_ORIENTATION_VERTICAL;
        int track = vertical ? h : w;
        int thumb_start = 0, thumb_len = 0;

        if (adj) {
            double lower = gtk_adjustment_get_lower(adj);
            double upper = gtk_adjustment_get_upper(adj);
            double page = gtk_adjustment_get_page_size(adj);
            double value = gtk_adjustment_get_value(adj);
            double range = upper - lower;
            if (range > 0 && page > 0 && page < range) {
                double frac = (value - lower) / (range - page);
                thumb_len = (int) round(track * page / range);
                if (thumb_len < 16) thumb_len = 16;
                if (thumb_len > track) thumb_len = track;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                thumb_start = (int) round(frac * (track - thumb_len));
                extra.vertical = vertical;
            } else {
                thumb_len = track; /* everything fits: full-length scrubber */
            }
        }
        rc = s_paint_scrollbar ? s_paint_scrollbar(w, h, px, vertical,
                                                   thumb_start, thumb_len,
                                                   (state & SERENITY_STATE_PRESSED) ? 1 : 0,
                                                   (state & SERENITY_STATE_HOVER) ? 1 : 0)
                               : -1;
    } else {
        rc = s_paint_widget(w, h, px, (int) kind, (int) state,
                            extra.vertical, extra.value, extra.max_value,
                            (int) extra.arrow);
    }
    if (rc != 0) {
        cairo_surface_destroy(surf);
        return FALSE;
    }
    cairo_surface_mark_dirty(surf);

    cr = gtk_snapshot_append_cairo(snapshot, &r);
    if (cr) {
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    cairo_surface_destroy(surf);
    return TRUE;
}

gboolean _gtk_serenity_suppress_background(void)
{
    if (!serenity_ui_is_active()) return FALSE;
    if (s_depth < 1 || s_depth > MAX_DEPTH) return FALSE;
    return s_suppress_stack[s_depth - 1];
}

gboolean _gtk_serenity_suppress_border(void)
{
    return _gtk_serenity_suppress_background();
}
