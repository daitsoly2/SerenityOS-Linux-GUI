/* GTK3 -> Serenity widget rendering bridge.
 *
 * GTK3 has one central rendering path: gtk_render_background()/frame()/check()
 * /option()/slider()/handle()/arrow() in gtkrender.c. Every widget goes
 * through it, so replacing those calls replaces the *widget itself*, not
 * just its colours. All we need in order to know which widget we are painting
 * is the innermost element name of the GtkStyleContext's widget path.
 *
 * The pixels come from Serenity's REAL Gfx::StylePainter via
 * libserenitypaint.so (dlopen'd, C ABI), so no cairo reimplementation of the
 * classic look exists anywhere in the tree.
 *
 * Nothing here changes a public GTK API: this file is compiled into
 * libgtk-3.so.0 with hidden visibility and exports no symbols.
 */
#include "config.h"
#include "gtkserenity.h"

#include "gtkwidgetpath.h"
#include "gtkcssprovider.h"
#include "gtkstyleprovider.h"
#include "gtksettingsprivate.h"
#include "gtkwidget.h"

#include "serenity-ui.h"

#include <cairo.h>
#include <dlfcn.h>
#include <string.h>

typedef int (*serenity_paint_widget_fn)(int, int, unsigned char *, int, int,
                                        int, double, int, int);
typedef int (*serenity_paint_arrow_fn)(int, int, unsigned char *, int, int);
static serenity_paint_widget_fn s_paint_widget;
static serenity_paint_arrow_fn  s_paint_arrow;
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
        s_paint_arrow  = (serenity_paint_arrow_fn) dlsym(h, "serenity_paint_arrow");
    }
}

static GtkCssProvider *s_provider;
static guint64 s_css_revision;
static gboolean s_inited;

static const struct {
    const char *element;
    SerenityWidgetKind kind;
} s_element_map[] = {
    { "button",          SERENITY_WIDGET_BUTTON },
    { "togglebutton",    SERENITY_WIDGET_TOGGLE_BUTTON },
    { "radiobutton",     SERENITY_WIDGET_TOGGLE_BUTTON },
    { "checkbutton",     SERENITY_WIDGET_CHECK },
    { "entry",           SERENITY_WIDGET_ENTRY },
    { "spinbutton",      SERENITY_WIDGET_ENTRY },
    { "searchentry",     SERENITY_WIDGET_ENTRY },
    { "textview",        SERENITY_WIDGET_ENTRY },
    { "combobox",        SERENITY_WIDGET_COMBO },
    { "comboboxentry",   SERENITY_WIDGET_COMBO },
    { "frame",           SERENITY_WIDGET_FRAME_RAISED },
    { "aspectframe",     SERENITY_WIDGET_FRAME_RAISED },
    { "scrolledwindow",  SERENITY_WIDGET_FRAME_SUNKEN },
    { "scrollbar",       SERENITY_WIDGET_SCROLLBAR_TROUGH },
    { "trough",          SERENITY_WIDGET_SCROLLBAR_TROUGH },
    { "slider",          SERENITY_WIDGET_SCROLLBAR_THUMB },
    /* The trough only: GTK3 paints the filled part itself (render_activity),
     * so a base-coloured sunken well is left underneath it. */
    { "progressbar",     SERENITY_WIDGET_FRAME_SUNKEN },
    { "scale",           SERENITY_WIDGET_SCROLLBAR_TROUGH },
    { "headerbar",       SERENITY_WIDGET_HEADER_BAR },
    { "toolbar",         SERENITY_WIDGET_TOOLBAR },
    { "menubar",         SERENITY_WIDGET_MENU_BAR },
    { "menu",            SERENITY_WIDGET_MENU },
    { "menuitem",        SERENITY_WIDGET_MENU_ITEM },
    { "modelbutton",     SERENITY_WIDGET_MENU_ITEM },
    { "popover",         SERENITY_WIDGET_MENU },
    { "statusbar",       SERENITY_WIDGET_STATUS_BAR },
    { "notebook",        SERENITY_WIDGET_TAB },
    { "treeview",        SERENITY_WIDGET_ENTRY },
    { "iconview",        SERENITY_WIDGET_ENTRY },
    { "list",            SERENITY_WIDGET_ENTRY },
    { "separator",       SERENITY_WIDGET_SEPARATOR },
    { "paned",           SERENITY_WIDGET_HANDLE },
    { "dialog",          SERENITY_WIDGET_FRAME_RAISED },
};

static SerenityWidgetKind kind_of_node_name(const char *name)
{
    size_t i;
    if (!name) return SERENITY_WIDGET_NONE;
    for (i = 0; i < G_N_ELEMENTS(s_element_map); i++)
        if (g_ascii_strcasecmp(name, s_element_map[i].element) == 0)
            return s_element_map[i].kind;
    return SERENITY_WIDGET_NONE;
}

static SerenityWidgetKind kind_of(GtkStyleContext *context)
{
    const GtkWidgetPath *path;
    const char *name;
    int pos;

    if (!serenity_ui_is_active()) return SERENITY_WIDGET_NONE;
    path = gtk_style_context_get_path(context);
    if (!path) return SERENITY_WIDGET_NONE;
    pos = gtk_widget_path_length(path) - 1;
    if (pos < 0) return SERENITY_WIDGET_NONE;
    /* The CSS element name is what maps to a widget kind ("button", "entry",
     * "menuitem", ...). gtk_widget_path_iter_get_object_name() is the widget's
     * *name* property and is usually NULL, which is why header-bar buttons used
     * to keep their stock look. */
    name = gtk_widget_path_iter_get_name(path, pos);
    if (!name)
        name = gtk_widget_path_iter_get_object_name(path, pos);
    return kind_of_node_name(name);
}

static SerenityWidgetState state_of(GtkStyleContext *context)
{
    GtkStateFlags f = gtk_style_context_get_state(context);
    SerenityWidgetState s = SERENITY_STATE_NORMAL;
    if (f & GTK_STATE_FLAG_ACTIVE)     s |= SERENITY_STATE_PRESSED;
    if (f & GTK_STATE_FLAG_PRELIGHT)   s |= SERENITY_STATE_HOVER;
    if (f & GTK_STATE_FLAG_SELECTED)   s |= SERENITY_STATE_SELECTED;
    if (f & GTK_STATE_FLAG_CHECKED)    s |= SERENITY_STATE_CHECKED;
    if (f & GTK_STATE_FLAG_INSENSITIVE)s |= SERENITY_STATE_DISABLED;
    if (f & GTK_STATE_FLAG_FOCUSED)    s |= SERENITY_STATE_FOCUSED;
    if (f & GTK_STATE_FLAG_BACKDROP)   s |= SERENITY_STATE_BACKDROP;
    return s;
}

static gboolean reload_theme(gpointer data)
{
    guint64 rev;
    (void)data;
    if (!s_provider) return G_SOURCE_CONTINUE;
    rev = serenity_ui_revision();
    if (rev != s_css_revision) {
        gtk_css_provider_load_from_data(s_provider, serenity_ui_css(3), -1, NULL);
        s_css_revision = rev;
    }
    return G_SOURCE_CONTINUE;
}

void _gtk_serenity_init(GdkScreen *screen)
{
    if (s_inited || !screen) return;
    s_inited = TRUE;
    /* SERENITY_GTK_LEGACY=1 must give stock Adwaita, so the stylesheet that
     * blanks the CSS backgrounds we replace must not be installed. */
    if (!serenity_ui_is_active()) return;
    s_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(s_provider, serenity_ui_css(3), -1, NULL);
    s_css_revision = serenity_ui_revision();
    gtk_style_context_add_provider_for_screen(screen,
                                              GTK_STYLE_PROVIDER(s_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_timeout_add(1000, reload_theme, NULL);
}

static void ensure_init(void)
{
    if (!s_inited) _gtk_serenity_init(gdk_screen_get_default());
}

/* Paint `kind` into the given rectangle of the cairo context, using Serenity's
 * real painter. Returns FALSE when the bridge is unavailable so the caller
 * can fall back to GTK's own rendering. */
static gboolean paint_kind(cairo_t *cr, double x, double y,
                           double width, double height,
                           SerenityWidgetKind kind, SerenityWidgetState state,
                           const SerenityWidgetExtra *extra)
{
    int w = (int)width;
    int h = (int)height;
    unsigned char *px;
    cairo_surface_t *surf;
    gboolean ok = FALSE;

    if (!cr || w < 1 || h < 1) return FALSE;
    load_bridge();
    if (!s_paint_widget) return FALSE;

    px = g_malloc0((gsize) w * h * 4);
    if (s_paint_widget(w, h, px, (int) kind, (int) state,
                       extra ? extra->vertical : 0,
                       extra ? extra->value : 0.0,
                       extra ? extra->max_value : 0,
                       extra ? (int) extra->arrow : 0) == 0) {
        cairo_save(cr);
        surf = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, w, h, w * 4);
        cairo_set_source_surface(cr, surf, x, y);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
        cairo_restore(cr);
        ok = TRUE;
    }
    g_free(px);
    return ok;
}

gboolean _gtk_serenity_render_background(GtkStyleContext *context, cairo_t *cr,
                                         gdouble x, gdouble y,
                                         gdouble width, gdouble height)
{
    SerenityWidgetKind kind;
    ensure_init();
    kind = kind_of(context);
    if (kind == SERENITY_WIDGET_NONE) return FALSE;
    return paint_kind(cr, x, y, width, height, kind, state_of(context), NULL);
}

/* The frame was already painted as part of the widget above. Suppressing it
 * is what stops GTK from drawing Adwaita's rounded border over our bevel. */
gboolean _gtk_serenity_render_frame(GtkStyleContext *context, cairo_t *cr,
                                    gdouble x, gdouble y,
                                    gdouble width, gdouble height)
{
    (void)cr; (void)x; (void)y; (void)width; (void)height;
    ensure_init();
    return kind_of(context) != SERENITY_WIDGET_NONE;
}

gboolean _gtk_serenity_render_check(GtkStyleContext *context, cairo_t *cr,
                                    gdouble x, gdouble y,
                                    gdouble width, gdouble height)
{
    ensure_init();
    if (!serenity_ui_is_active()) return FALSE;
    return paint_kind(cr, x, y, width, height, SERENITY_WIDGET_CHECK,
                      state_of(context), NULL);
}

gboolean _gtk_serenity_render_option(GtkStyleContext *context, cairo_t *cr,
                                     gdouble x, gdouble y,
                                     gdouble width, gdouble height)
{
    ensure_init();
    if (!serenity_ui_is_active()) return FALSE;
    return paint_kind(cr, x, y, width, height, SERENITY_WIDGET_RADIO,
                      state_of(context), NULL);
}

gboolean _gtk_serenity_render_slider(GtkStyleContext *context, cairo_t *cr,
                                     gdouble x, gdouble y,
                                     gdouble width, gdouble height,
                                     GtkOrientation orientation)
{
    SerenityWidgetExtra extra = { SERENITY_ARROW_NONE, 0.5,
                                  orientation == GTK_ORIENTATION_VERTICAL, 0, NULL };
    ensure_init();
    if (!serenity_ui_is_active()) return FALSE;
    return paint_kind(cr, x, y, width, height, SERENITY_WIDGET_SCROLLBAR_THUMB,
                      state_of(context), &extra);
}

gboolean _gtk_serenity_render_handle(GtkStyleContext *context, cairo_t *cr,
                                     gdouble x, gdouble y,
                                     gdouble width, gdouble height)
{
    ensure_init();
    if (!serenity_ui_is_active()) return FALSE;
    return paint_kind(cr, x, y, width, height, SERENITY_WIDGET_HANDLE,
                      state_of(context), NULL);
}

gboolean _gtk_serenity_render_arrow(GtkStyleContext *context, cairo_t *cr,
                                    gdouble angle, gdouble x, gdouble y,
                                    gdouble size)
{
    SerenityArrow arrow;
    unsigned char *px;
    cairo_surface_t *surf;
    double r = angle * 180.0 / G_PI;
    int w, h;

    ensure_init();
    if (!serenity_ui_is_active()) return FALSE;
    if (r < 45 || r >= 315)      arrow = SERENITY_ARROW_UP;
    else if (r < 135)            arrow = SERENITY_ARROW_RIGHT;
    else if (r < 225)            arrow = SERENITY_ARROW_DOWN;
    else                         arrow = SERENITY_ARROW_LEFT;

    load_bridge();
    if (!s_paint_arrow) return FALSE;

    w = h = (int)(size < 9 ? 9 : size);
    px = g_malloc0((gsize) w * h * 4);
    if (s_paint_arrow(w, h, px, (int) arrow,
                      (gtk_style_context_get_state(context) & GTK_STATE_FLAG_INSENSITIVE) ? 0 : 1) != 0) {
        g_free(px);
        return FALSE;
    }
    cairo_save(cr);
    surf = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    cairo_set_source_surface(cr, surf, x - w / 2, y - h / 2);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
    cairo_restore(cr);
    g_free(px);
    return TRUE;
}

/* Gadgets never reach gtk_render_background(): they paint their CSS box
 * straight from gtk_css_gadget_draw(). That path carries every menu entry
 * (GtkMenuItem, GtkModelButton) and plain buttons inside popovers, so the
 * popups were the one surface that stayed stock-Adwaita. Take over exactly
 * those boxes here; everything outside a menu/popover keeps the CSS look. */
gboolean _gtk_serenity_render_gadget(GtkWidget *owner, const gchar *node_name,
                                     GtkStateFlags f, cairo_t *cr,
                                     gdouble x, gdouble y,
                                     gdouble width, gdouble height)
{
    SerenityWidgetKind kind;
    SerenityWidgetState state;
    GType t;

    if (!serenity_ui_is_active()) return FALSE;
    if (!owner || !node_name || !cr) return FALSE;
    if (width < 1 || height < 1) return FALSE;

    /* Only widgets living inside a classic menu or a popover are taken over.
     * Menubar items must stay flat on the bar, so GtkMenuBar is NOT an
     * acceptable ancestor. */
    t = g_type_from_name("GtkMenu");
    if (t == G_TYPE_INVALID || !gtk_widget_get_ancestor(owner, t)) {
        t = g_type_from_name("GtkPopover");
        if (t == G_TYPE_INVALID || !gtk_widget_get_ancestor(owner, t))
            return FALSE;
    }

    kind = kind_of_node_name(node_name);
    /* Buttons inside a menu are menu items: gedit's icon row must not be a
     * row of beveled toolbar buttons. */
    if (kind == SERENITY_WIDGET_BUTTON ||
        kind == SERENITY_WIDGET_TOGGLE_BUTTON ||
        kind == SERENITY_WIDGET_COMBO)
        kind = SERENITY_WIDGET_MENU_ITEM;

    if (g_getenv("SERENITY_DEBUG_KIND"))
        g_printerr("SERENITY_GADGET node=%s owner=%s flags=%d kind=%d\n",
                   node_name, G_OBJECT_TYPE_NAME(owner), (int) f, (int) kind);

    if (kind != SERENITY_WIDGET_MENU_ITEM)
        return FALSE;

    state = SERENITY_STATE_NORMAL;
    if (f & GTK_STATE_FLAG_ACTIVE)      state |= SERENITY_STATE_PRESSED;
    if (f & GTK_STATE_FLAG_PRELIGHT)    state |= SERENITY_STATE_HOVER;
    if (f & GTK_STATE_FLAG_SELECTED)    state |= SERENITY_STATE_SELECTED;
    if (f & GTK_STATE_FLAG_CHECKED)     state |= SERENITY_STATE_CHECKED;
    if (f & GTK_STATE_FLAG_INSENSITIVE) state |= SERENITY_STATE_DISABLED;

    if (state & (SERENITY_STATE_HOVER | SERENITY_STATE_PRESSED |
                 SERENITY_STATE_CHECKED | SERENITY_STATE_SELECTED))
        return paint_kind(cr, x, y, width, height,
                          SERENITY_WIDGET_MENU_SELECTION, state, NULL);

    /* Idle item: suppress the stock background and border entirely, so the
     * menu panel's own base colour and stripe show through pixel-aligned. */
    return TRUE;
}
