/*
 * im-serenity.c -- GTK3 input method module that routes key events through
 * the Serenity IME bridge instead of fcitx5's stock GTK frontend.
 *
 * Why this exists
 * ---------------
 * On this port, Serenity-native (LibGUI) apps talk to fcitx5 through the
 * `serenityime` addon over /tmp/serenity-ime.sock. That addon uses fcitx5's
 * ClientSideUI capability, so the *client* draws the preedit and the candidate
 * window. GTK/Qt apps normally miss out: they load im-fcitx5.so and get
 * fcitx5's own UI. This module makes a GTK3 app behave like a LibGUI client:
 *
 *   GtkTextView -> SerenityIMContext -> /tmp/serenity-ime.sock
 *                                   -> serenityime addon -> fcitx5 (rime)
 *
 * Consequences:
 *   - Key events are consumed/committed by the same engine as native apps.
 *   - The pinyin preedit is delivered through GtkIMContext::get_preedit_string,
 *     so GTK draws it *inline in the app's input area* (underlined), matching
 *     the native TextEditor::set_preedit behaviour.
 *   - The candidate list is painted by us in a GTK popup using the exact
 *     palette/layout from LibGUI/InputMethodCandidateWindow.cpp, so the
 *     candidate window looks identical to the Serenity one.
 *
 * Honest limitation: a GTK process links against GTK/Wayland, not LibGUI, so
 * it cannot create a GUI::Window (a WindowServer IPC object). The candidate
 * window here is a GTK popup that *replicates* the Serenity visual language,
 * not the literal GUI::Window class. See README.md "Linux apps" section.
 *
 * Build / activation
 * ------------------
 *   cc -shared -fPIC -o im-serenity.so im-serenity.c $(pkg-config --cflags --libs gtk+-3.0)
 *   GTK_IM_MODULE=serenity GTK_IM_MODULE_FILE=<cache> <gtk3-app>
 *
 * The addon accepts exactly one client. Like the LibGUI client, this module
 * connects on focus_in and disconnects on focus_out, so the focused app owns
 * the socket and switching focus hands it over cleanly.
 */

#include <gtk/gtk.h>
#include <gtk/gtkimmodule.h>
#include <gtk/gtkimcontextinfo.h>
#include <gdk/gdkkeysyms.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- Serenity candidate-window palette (keep in sync with
 *      LibGUI/InputMethodCandidateWindow.cpp paint_event) ---- */
#define SR_SHADOW         0x000000 /* used with alpha 0x33 */
#define SR_BACKGROUND     0xe8edf3
#define SR_BORDER         0xb6c2d4
#define SR_TEXT_NORMAL    0x1f2a37
#define SR_TEXT_DIM       0x6b7785
#define SR_SELECTED_FILL  0x2f6fd0
#define SR_SELECTED_TEXT  0xffffff
#define SR_SELECTED_BORDER 0x2560b8
#define SR_SEPARATOR      0xd5dde6

static const char* default_socket_path = "/tmp/serenity-ime.sock";

static void log_msg(const char* fmt, ...)
{
    if (!getenv("SERENITY_GTK_IM_DEBUG"))
        return;
    FILE* f = fopen("/tmp/serenity-gtk-im.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------- type ------------------------------- */

typedef struct _SerenityIMContext SerenityIMContext;
typedef struct _SerenityIMContextClass SerenityIMContextClass;

struct _SerenityIMContextClass {
    GtkIMContextClass parent_class;
};

struct _SerenityIMContext {
    GtkIMContext parent_instance;

    int fd;
    GString* rx;                 /* bytes received but not yet a full line */
    GdkWindow* client_window;
    GdkRectangle cursor_rect;
    gboolean have_cursor_rect;

    char* preedit;               /* UTF-8, owned */
    int preedit_cursor;          /* in characters */
    gboolean preedit_shown;      /* preedit-start emitted */

    char* aux;                   /* UTF-8, owned */
    char** labels;               /* UTF-8 array, owned */
    char** texts;
    int candidate_count;
    int candidate_cursor;
    int has_prev;
    int has_next;

    GtkWidget* popup;            /* candidate window (GTK_WINDOW_POPUP) */
    gboolean use_preedit;
};

G_DEFINE_DYNAMIC_TYPE(SerenityIMContext, serenity_im_context, GTK_TYPE_IM_CONTEXT)

/* --------------------------- small helpers --------------------------- */

static const char* socket_path(void)
{
    const char* from_env = getenv("SERENITY_IME_SOCKET");
    return (from_env && *from_env) ? from_env : default_socket_path;
}

static void rgb(cairo_t* cr, unsigned int color)
{
    cairo_set_source_rgb(cr,
        ((color >> 16) & 0xff) / 255.0,
        ((color >> 8) & 0xff) / 255.0,
        (color & 0xff) / 255.0);
}

/* Decode the addon's hex-encoded UTF-8 fields. Returns a newly allocated
 * (possibly empty) UTF-8 string. Never returns NULL. */
static char* hex_decode(const char* hex)
{
    if (!hex)
        return g_strdup("");
    size_t n = strlen(hex) / 2;
    char* out = g_malloc0(n + 1);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        const char* hi = strchr(digits, g_ascii_tolower(hex[2 * i]));
        const char* lo = strchr(digits, g_ascii_tolower(hex[2 * i + 1]));
        if (!hi || !lo)
            break;
        out[i] = (char)(((hi - digits) << 4) | (lo - digits));
    }
    return out;
}

static char* hex_encode(const char* text)
{
    if (!text)
        return g_strdup("");
    static const char digits[] = "0123456789abcdef";
    size_t n = strlen(text);
    char* out = g_malloc0(n * 2 + 1);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        out[2 * i] = digits[c >> 4];
        out[2 * i + 1] = digits[c & 0xf];
    }
    return out;
}

static void send_line(SerenityIMContext* ctx, const char* line)
{
    if (ctx->fd < 0)
        return;
    size_t len = strlen(line);
    char* buf = g_malloc(len + 2);
    memcpy(buf, line, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    ssize_t written = 0;
    while ((size_t)written < len + 1) {
        ssize_t n = send(ctx->fd, buf + written, len + 1 - written, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        written += n;
    }
    g_free(buf);
    log_msg("-> %s", line);
}

/* Split on a single space, PRESERVING empty fields (aux can be ""). */
static char** split_spaces(const char* line)
{
    return g_strsplit(line, " ", -1);
}

static void free_candidates(SerenityIMContext* ctx)
{
    if (ctx->labels)
        g_strfreev(ctx->labels);
    if (ctx->texts)
        g_strfreev(ctx->texts);
    ctx->labels = NULL;
    ctx->texts = NULL;
    ctx->candidate_count = 0;
    ctx->candidate_cursor = -1;
}

/* --------------------------- candidate popup --------------------------- */

typedef struct {
    char* label;
    char* text;
    int x;
    int width;
} CandidateItem;

static gboolean on_popup_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);

static void popup_relayout(SerenityIMContext* ctx)
{
    if (!ctx->popup)
        return;

    /* Measure with a scratch Pango layout using the widget's context. */
    PangoLayout* layout = gtk_widget_create_pango_layout(ctx->popup, "Mg");
    PangoFontDescription* font = pango_font_description_from_string("Sans 13");
    pango_layout_set_font_description(layout, font);

    int font_h = 0;
    pango_layout_get_pixel_size(layout, NULL, &font_h);
    int const row_h = font_h + 8;
    int const pad = 6;
    int const separator_w = 1;

    int width = pad;
    if (ctx->aux && *ctx->aux) {
        pango_layout_set_text(layout, ctx->aux, -1);
        int w = 0;
        pango_layout_get_pixel_size(layout, &w, NULL);
        width += w + pad;
    }
    for (int i = 0; i < ctx->candidate_count; ++i) {
        char* joined = g_strconcat(ctx->labels[i], ctx->texts[i], NULL);
        pango_layout_set_text(layout, joined, -1);
        int w = 0;
        pango_layout_get_pixel_size(layout, &w, NULL);
        g_free(joined);
        width += w + pad * 2;
        if (i != ctx->candidate_count - 1)
            width += separator_w;
    }
    /* Paging indicators: measure the actual glyphs we draw in on_popup_draw.
     * Reserving 2*(font_h+pad) used to leave ~30px of empty panel after ">". */
    int prev_w = 0;
    int next_w = 0;
    pango_layout_set_text(layout, ctx->has_prev ? "<" : " ", -1);
    pango_layout_get_pixel_size(layout, &prev_w, NULL);
    pango_layout_set_text(layout, ctx->has_next ? ">" : " ", -1);
    pango_layout_get_pixel_size(layout, &next_w, NULL);
    width += prev_w + 2 + next_w + pad;

    int height = row_h + 4;      /* +4: 1px top + 2px shadow + slack */
    pango_font_description_free(font);
    g_object_unref(layout);

    gtk_widget_set_size_request(ctx->popup, width + 2, height + 2);
    gtk_widget_queue_draw(ctx->popup);
}

static void popup_ensure(SerenityIMContext* ctx)
{
    if (ctx->popup)
        return;
    ctx->popup = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(ctx->popup), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_widget_set_app_paintable(ctx->popup, TRUE);
    gtk_window_set_decorated(GTK_WINDOW(ctx->popup), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(ctx->popup), FALSE);
    gtk_widget_add_events(ctx->popup, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(ctx->popup, "draw", G_CALLBACK(on_popup_draw), ctx);
    g_object_set_data(G_OBJECT(ctx->popup), "serenity-im-context", ctx);
}

static void popup_hide(SerenityIMContext* ctx)
{
    if (ctx->popup && gtk_widget_get_visible(ctx->popup))
        gtk_widget_hide(ctx->popup);
}

/* Find the GtkWindow that owns the app surface, so the candidate popup can be
 * made transient for it. Wayland needs a real parent xdg_surface: without one
 * GDK falls back to an unparented toplevel and move_to_rect() does nothing. */
static GtkWidget* resolve_toplevel(GdkWindow* window)
{
    if (!window)
        return NULL;
    GdkWindow* top = gdk_window_get_toplevel(window);
    gpointer user_data = NULL;
    gdk_window_get_user_data(top, &user_data);
    if (user_data && GTK_IS_WINDOW(user_data))
        return GTK_WIDGET(user_data);
    return NULL;
}

/* Translate the IM cursor rectangle (relative to client_window) into the
 * toplevel surface coordinate space that gdk_window_move_to_rect expects. */
static GdkRectangle cursor_rect_for_toplevel(SerenityIMContext* ctx)
{
    GdkRectangle rect = ctx->cursor_rect;
    if (!ctx->have_cursor_rect) {
        rect.x = 0;
        rect.y = 0;
        rect.width = 1;
        rect.height = 1;
    }
    if (rect.width <= 0)
        rect.width = 1;
    if (rect.height <= 0)
        rect.height = 1;

    if (ctx->client_window) {
        GdkWindow* top = gdk_window_get_toplevel(ctx->client_window);
        if (top && top != ctx->client_window) {
            int cx = 0, cy = 0, tx = 0, ty = 0;
            gdk_window_get_root_coords(ctx->client_window, rect.x, rect.y, &cx, &cy);
            gdk_window_get_root_coords(top, 0, 0, &tx, &ty);
            rect.x = cx - tx;
            rect.y = cy - ty;
        }
    }
    return rect;
}

static void popup_show_or_hide(SerenityIMContext* ctx)
{
    if (ctx->candidate_count <= 0) {
        popup_hide(ctx);
        return;
    }
    popup_ensure(ctx);
    popup_relayout(ctx);

    /* IMPORTANT: do NOT use gtk_widget_set_parent_window() here. On the
     * Wayland backend that makes the popup a wl_subsurface of the app's
     * surface: it gets clipped to the parent geometry and GDK's
     * move_to_rect() takes the subsurface branch, so no xdg_popup is ever
     * created and the candidate window is invisible. A transient_for
     * relationship instead gives GDK a parent xdg_surface to attach a real
     * xdg_popup (with a positioner) to. */
    GtkWidget* toplevel = resolve_toplevel(ctx->client_window);
    if (toplevel && toplevel != ctx->popup)
        gtk_window_set_transient_for(GTK_WINDOW(ctx->popup), GTK_WINDOW(toplevel));

    GdkRectangle rect = cursor_rect_for_toplevel(ctx);

    /* GDK builds the xdg_positioner when the popup is first mapped. If we only
     * call move_to_rect() after show_all(), the initial xdg_popup is created
     * with an anchor_rect of (0,0) (the compositor then reports a geometry of
     * -26,-23, i.e. just the CSD offset) and our call turns into an
     * xdg_popup.reposition that never completes -> the candidate window stays
     * pinned to the parent window's top-left decoration. Realize the widget
     * first and hand GDK the caret rect BEFORE mapping, so the very first
     * positioner already carries the correct anchor. This is safe now that
     * gtk_widget_set_parent_window() is gone (that was what made an explicit
     * realize fail the "anchored" assertion). */
    gboolean first_map = !gtk_widget_get_realized(ctx->popup);
    if (first_map) {
        gtk_widget_realize(ctx->popup);
        GdkWindow* pending_win = gtk_widget_get_window(ctx->popup);
        if (pending_win) {
            gdk_window_move_to_rect(pending_win, &rect,
                GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST,
                GDK_ANCHOR_SLIDE_X, 0, 2);
        }
    }

    if (!gtk_widget_get_visible(ctx->popup))
        gtk_widget_show_all(ctx->popup);

    GdkWindow* win = gtk_widget_get_window(ctx->popup);
    log_msg("popup n=%d client=%p toplevel=%p realized=%d visible=%d mapped=%d "
            "win=%p type=%d rect=%d,%d %dx%d",
        ctx->candidate_count, ctx->client_window, toplevel,
        gtk_widget_get_realized(ctx->popup), gtk_widget_get_visible(ctx->popup),
        win ? gdk_window_is_visible(win) : -1, win,
        win ? (int)gdk_window_get_window_type(win) : -1,
        rect.x, rect.y, rect.width, rect.height);

    if (win) {
        /* Wayland clients have no global screen coordinates; the compositor
         * positions xdg_popup windows from this surface-relative rect. */
        /* No FLIP_Y: our wlroots bridge applies the parent-window constraint
         * box in surface coordinates, and flipping there pushes the popup up
         * onto the window decoration. Keep it below the caret, slide only on
         * the X axis so a long 9-candidate row still fits on screen. */
        gdk_window_move_to_rect(win, &rect,
            GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST,
            GDK_ANCHOR_SLIDE_X, 0, 2);
    }
}

static gboolean on_popup_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
    SerenityIMContext* ctx = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    int const w = alloc.width;
    int const h = alloc.height;
    int const panel_w = w - 2;
    int const panel_h = h - 2;
    if (panel_w <= 0 || panel_h <= 0)
        return FALSE;

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans 13");
    pango_layout_set_font_description(layout, font);

    int font_h = 0;
    pango_layout_set_text(layout, "Mg", -1);
    pango_layout_get_pixel_size(layout, NULL, &font_h);
    int const pad = 6;
    int const row_y = 1 + (panel_h - font_h) / 2;

    /* shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0x33 / 255.0);
    cairo_rectangle(cr, 2, 2, panel_w, panel_h);
    cairo_fill(cr);

    /* panel */
    rgb(cr, SR_BACKGROUND);
    cairo_rectangle(cr, 0, 0, panel_w, panel_h);
    cairo_fill(cr);
    rgb(cr, SR_BORDER);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, panel_w - 1, panel_h - 1);
    cairo_stroke(cr);

    /* clip to panel so text never spills into the shadow */
    cairo_save(cr);
    cairo_rectangle(cr, 1, 1, panel_w - 2, panel_h - 2);
    cairo_clip(cr);

    int x = pad;

    if (ctx->aux && *ctx->aux) {
        int tw = 0;
        pango_layout_set_text(layout, ctx->aux, -1);
        pango_layout_get_pixel_size(layout, &tw, NULL);
        rgb(cr, SR_TEXT_DIM);
        cairo_move_to(cr, x, row_y);
        pango_cairo_show_layout(cr, layout);
        x += tw + pad;
    }

    for (int i = 0; i < ctx->candidate_count; ++i) {
        char* joined = g_strconcat(ctx->labels[i], ctx->texts[i], NULL);
        int tw = 0;
        pango_layout_set_text(layout, joined, -1);
        pango_layout_get_pixel_size(layout, &tw, NULL);
        int const item_w = tw + pad * 2;
        gboolean const selected = (i == ctx->candidate_cursor);

        if (selected) {
            rgb(cr, SR_SELECTED_FILL);
            cairo_rectangle(cr, x, 1, item_w, panel_h - 2);
            cairo_fill(cr);
            rgb(cr, SR_SELECTED_BORDER);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, x + 0.5, 1.5, item_w - 1, panel_h - 3);
            cairo_stroke(cr);
            rgb(cr, SR_SELECTED_TEXT);
        } else {
            rgb(cr, SR_TEXT_NORMAL);
        }

        cairo_move_to(cr, x + pad, row_y);
        pango_cairo_show_layout(cr, layout);
        x += item_w;
        g_free(joined);

        if (!selected && i != ctx->candidate_count - 1) {
            rgb(cr, SR_SEPARATOR);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, x + 0.5, 1 + 3);
            cairo_line_to(cr, x + 0.5, panel_h - 1 - 3);
            cairo_stroke(cr);
            x += 1;
        } else {
            x += 1;
        }
    }

    /* paging indicators */
    rgb(cr, (ctx->has_prev || ctx->has_next) ? SR_TEXT_NORMAL : SR_TEXT_DIM);
    pango_layout_set_text(layout, ctx->has_prev ? "<" : " ", -1);
    cairo_move_to(cr, x, row_y);
    pango_cairo_show_layout(cr, layout);
    int arrow_w = 0;
    pango_layout_get_pixel_size(layout, &arrow_w, NULL);
    x += arrow_w + 2;
    pango_layout_set_text(layout, ctx->has_next ? ">" : " ", -1);
    cairo_move_to(cr, x, row_y);
    pango_cairo_show_layout(cr, layout);

    cairo_restore(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
    return FALSE;
}

/* --------------------------- protocol client --------------------------- */

static gboolean read_one_line(SerenityIMContext* ctx, char** out, int timeout_ms)
{
    for (;;) {
        if (ctx->rx) {
            char* nl = memchr(ctx->rx->str, '\n', ctx->rx->len);
            if (nl) {
                size_t len = (size_t)(nl - ctx->rx->str);
                *out = g_strndup(ctx->rx->str, len);
                g_string_erase(ctx->rx, 0, len + 1);
                return TRUE;
            }
        }
        struct pollfd pfd = { ctx->fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0)
            return FALSE;
        char buf[4096];
        ssize_t n = recv(ctx->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return FALSE;
        if (!ctx->rx)
            ctx->rx = g_string_new_len(NULL, 0);
        g_string_append_len(ctx->rx, buf, n);
    }
}

static void handle_panel(SerenityIMContext* ctx, char** parts, int count)
{
    /* panel <preedit_cursor> <aux_hex> <preedit_hex> <cand_cursor> <n>
     *       [<label_hex> <text_hex>]* <has_prev> <has_next>             */
    if (count < 8)
        return;

    char* new_preedit = hex_decode(parts[3]);
    int new_cursor = atoi(parts[1]);
    if (new_cursor < 0)
        new_cursor = 0;

    gboolean changed = !ctx->preedit || strcmp(ctx->preedit, new_preedit) != 0
        || ctx->preedit_cursor != new_cursor;
    g_free(ctx->preedit);
    ctx->preedit = new_preedit;
    ctx->preedit_cursor = new_cursor;

    g_free(ctx->aux);
    ctx->aux = hex_decode(parts[2]);

    int sel = atoi(parts[4]);
    int n = atoi(parts[5]);
    if (n < 0)
        n = 0;
    if (count < 6 + 2 * n + 2)
        n = 0;                   /* malformed: refuse to over-read */

    free_candidates(ctx);
    ctx->candidate_count = n;
    ctx->candidate_cursor = sel;
    ctx->labels = g_new0(char*, n + 1);
    ctx->texts = g_new0(char*, n + 1);
    for (int i = 0; i < n; ++i) {
        ctx->labels[i] = hex_decode(parts[6 + 2 * i]);
        ctx->texts[i] = hex_decode(parts[7 + 2 * i]);
    }
    ctx->has_prev = atoi(parts[6 + 2 * n]);
    ctx->has_next = atoi(parts[7 + 2 * n]);

    log_msg("panel preedit='%s' n=%d sel=%d", ctx->preedit, n, sel);

    /* Inline preedit: GTK draws this inside the app's input area. */
    if (changed || !ctx->preedit_shown) {
        if (!ctx->preedit_shown && ctx->preedit && *ctx->preedit) {
            g_signal_emit_by_name(ctx, "preedit-start");
            ctx->preedit_shown = TRUE;
        }
        g_signal_emit_by_name(ctx, "preedit-changed");
        if (ctx->preedit_shown && (!ctx->preedit || !*ctx->preedit)) {
            g_signal_emit_by_name(ctx, "preedit-end");
            ctx->preedit_shown = FALSE;
        }
    }

    popup_show_or_hide(ctx);
}

/* Returns handled; sets *forwarded when the engine asked us to replay. */
static gboolean pump_until_handled(SerenityIMContext* ctx, gboolean* forwarded)
{
    *forwarded = FALSE;
    gboolean handled = FALSE;
    int budget_ms = 300;

    for (int guard = 0; guard < 64; ++guard) {
        char* line = NULL;
        if (!read_one_line(ctx, &line, budget_ms))
            break;
        log_msg("<- %s", line);
        char** parts = split_spaces(line);
        int count = 0;
        while (parts[count])
            ++count;

        if (count > 0 && strcmp(parts[0], "handled") == 0) {
            handled = count > 1 && strcmp(parts[1], "1") == 0;
            g_strfreev(parts);
            g_free(line);
            break;
        } else if (count > 0 && strcmp(parts[0], "panel") == 0) {
            handle_panel(ctx, parts, count);
        } else if (count > 1 && strcmp(parts[0], "commit") == 0) {
            char* text = hex_decode(parts[1]);
            log_msg("commit '%s'", text);
            g_signal_emit_by_name(ctx, "commit", text);
            g_free(text);
        } else if (count > 2 && strcmp(parts[0], "delete") == 0) {
            int offset = atoi(parts[1]);
            int size = atoi(parts[2]);
            gboolean ok = FALSE;
            g_signal_emit_by_name(ctx, "delete-surrounding", offset, size, &ok);
        } else if (strcmp(parts[0], "forward") == 0) {
            *forwarded = TRUE;
        }
        /* "active", "ok", "err" are informational here. */
        g_strfreev(parts);
        g_free(line);
    }
    return handled;
}

static void connect_engine(SerenityIMContext* ctx)
{
    if (ctx->fd >= 0)
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("connect(%s) failed: %s", socket_path(), strerror(errno));
        close(fd);
        return;
    }
    ctx->fd = fd;
    if (ctx->rx)
        g_string_truncate(ctx->rx, 0);
    log_msg("connected to %s", socket_path());

    send_line(ctx, "create");
    /* Consume "ok" plus any initial active/status lines, briefly. */
    for (int i = 0; i < 4; ++i) {
        char* line = NULL;
        if (!read_one_line(ctx, &line, 60))
            break;
        log_msg("<- %s (init)", line);
        gboolean is_ok = strncmp(line, "ok", 2) == 0;
        g_free(line);
        if (is_ok)
            break;
    }
    send_line(ctx, "cap 0");
    send_line(ctx, "focus 1");
}

static void disconnect_engine(SerenityIMContext* ctx)
{
    if (ctx->fd < 0)
        return;
    send_line(ctx, "focus 0");
    send_line(ctx, "destroy");
    close(ctx->fd);
    ctx->fd = -1;
    if (ctx->rx)
        g_string_truncate(ctx->rx, 0);
    log_msg("disconnected");
}

static void push_cursor(SerenityIMContext* ctx)
{
    if (ctx->fd < 0 || !ctx->have_cursor_rect)
        return;
    int x = ctx->cursor_rect.x;
    int y = ctx->cursor_rect.y;
    if (ctx->client_window)
        gdk_window_get_root_coords(ctx->client_window, x, y, &x, &y);
    char* line = g_strdup_printf("cursor %d %d %d %d", x, y,
        ctx->cursor_rect.width, ctx->cursor_rect.height);
    send_line(ctx, line);
    g_free(line);
}

/* Keep surrounding text in sync; the addon needs it for deleteSurroundingText.
 * GTK reports byte offsets, the protocol wants character offsets. */
static void push_surround(SerenityIMContext* ctx)
{
    if (ctx->fd < 0)
        return;
    char* text = NULL;
    int cursor_index = 0;
    if (!gtk_im_context_get_surrounding(GTK_IM_CONTEXT(ctx), &text, &cursor_index) || !text)
        return;
    glong cursor_chars = g_utf8_strlen(text, cursor_index);
    char* hex = hex_encode(text);
    char* line = g_strdup_printf("surround %ld %ld %s",
        cursor_chars, cursor_chars, hex);
    send_line(ctx, line);
    g_free(line);
    g_free(hex);
    g_free(text);
}

/* --------------------------- GtkIMContext vfuncs --------------------------- */

static void impl_set_client_window(GtkIMContext* context, GdkWindow* window)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    if (ctx->client_window && ctx->client_window != window)
        popup_hide(ctx);
    ctx->client_window = window;
    /* Deliberately no gtk_widget_set_parent_window(): see the note in
     * popup_show_or_hide(). The transient_for relationship is refreshed on
     * every panel update, which also covers late client_window changes. */
}

static gboolean impl_filter_keypress(GtkIMContext* context, GdkEventKey* event)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    if (ctx->fd < 0)
        return FALSE;
    /* The addon protocol is press-only (LibGUI has no keyup hook either).
     * Forwarding releases would double every keystroke round trip and can
     * make fcitx re-emit a stale panel; let GTK handle them normally. */
    if (event->type == GDK_KEY_RELEASE)
        return FALSE;

    unsigned int states = 0;
    if (event->state & GDK_SHIFT_MASK) states |= 1;
    if (event->state & GDK_LOCK_MASK) states |= 2;
    if (event->state & GDK_CONTROL_MASK) states |= 4;
    if (event->state & GDK_MOD1_MASK) states |= 8;
    if (event->state & GDK_MOD4_MASK) states |= 64;
    int release = (event->type == GDK_KEY_RELEASE) ? 1 : 0;

    char* line = g_strdup_printf("key %u %u %d",
        (unsigned int)event->keyval, states, release);
    send_line(ctx, line);
    g_free(line);

    gboolean forwarded = FALSE;
    gboolean handled = pump_until_handled(ctx, &forwarded);
    push_surround(ctx);

    if (forwarded)
        return FALSE;              /* let GTK process the original key */
    return handled;
}

static void impl_reset(GtkIMContext* context)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    send_line(ctx, "reset");
    g_free(ctx->preedit);
    ctx->preedit = g_strdup("");
    ctx->preedit_cursor = 0;
    if (ctx->preedit_shown) {
        g_signal_emit_by_name(ctx, "preedit-end");
        ctx->preedit_shown = FALSE;
    }
    g_signal_emit_by_name(ctx, "preedit-changed");
    free_candidates(ctx);
    popup_hide(ctx);
}

static void impl_get_preedit_string(GtkIMContext* context, gchar** str,
    PangoAttrList** attrs, gint* cursor_pos)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    const char* text = ctx->preedit ? ctx->preedit : "";
    if (str)
        *str = g_strdup(text);
    if (attrs) {
        PangoAttrList* list = pango_attr_list_new();
        if (*text) {
            /* Solid underline = the "composing pinyin" marker, matching
             * TextEditor::paint_event's inline preedit. */
            PangoAttribute* underline = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            underline->start_index = 0;
            underline->end_index = strlen(text);
            pango_attr_list_insert(list, underline);
        }
        *attrs = list;
    }
    if (cursor_pos) {
        glong bytes = 0;
        if (*text) {
            const char* p = text;
            for (int i = 0; i < ctx->preedit_cursor && *p; ++i)
                p = g_utf8_next_char(p);
            bytes = p - text;
        }
        *cursor_pos = (gint)bytes;
    }
}

static void impl_focus_in(GtkIMContext* context)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    connect_engine(ctx);
    push_cursor(ctx);
    push_surround(ctx);
}

static void impl_focus_out(GtkIMContext* context)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    popup_hide(ctx);
    disconnect_engine(ctx);
    impl_reset(context);
}

static void impl_set_cursor_location(GtkIMContext* context, GdkRectangle* area)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    ctx->cursor_rect = *area;
    ctx->have_cursor_rect = TRUE;
    push_cursor(ctx);
    if (ctx->popup && gtk_widget_get_visible(ctx->popup))
        popup_show_or_hide(ctx);
}

static void impl_set_use_preedit(GtkIMContext* context, gboolean use_preedit)
{
    SerenityIMContext* ctx = (SerenityIMContext*)context;
    ctx->use_preedit = use_preedit;
}

static void impl_finalize(GObject* object)
{
    SerenityIMContext* ctx = (SerenityIMContext*)object;
    disconnect_engine(ctx);
    if (ctx->popup) {
        gtk_widget_destroy(ctx->popup);
        ctx->popup = NULL;
    }
    if (ctx->rx) {
        g_string_free(ctx->rx, TRUE);
        ctx->rx = NULL;
    }
    g_free(ctx->preedit);
    g_free(ctx->aux);
    free_candidates(ctx);
    G_OBJECT_CLASS(serenity_im_context_parent_class)->finalize(object);
}

/* --------------------------- type + module --------------------------- */

static void serenity_im_context_class_init(SerenityIMContextClass* klass)
{
    GtkIMContextClass* im_class = GTK_IM_CONTEXT_CLASS(klass);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    im_class->set_client_window = impl_set_client_window;
    im_class->filter_keypress = impl_filter_keypress;
    im_class->reset = impl_reset;
    im_class->get_preedit_string = impl_get_preedit_string;
    im_class->focus_in = impl_focus_in;
    im_class->focus_out = impl_focus_out;
    im_class->set_cursor_location = impl_set_cursor_location;
    im_class->set_use_preedit = impl_set_use_preedit;
    object_class->finalize = impl_finalize;
}

static void serenity_im_context_class_finalize(SerenityIMContextClass* klass)
{
    (void)klass;
}

static void serenity_im_context_init(SerenityIMContext* ctx)
{
    ctx->fd = -1;
    ctx->rx = g_string_new(NULL);
    ctx->preedit = g_strdup("");
    ctx->aux = g_strdup("");
    ctx->candidate_cursor = -1;
    ctx->use_preedit = TRUE;
}

/* GTK loads these four symbols from the .so listed in immodules.cache. */
static const GtkIMContextInfo serenity_info = {
    "serenity",                  /* context id -> GTK_IM_MODULE=serenity */
    "Serenity IME",              /* human readable name */
    "serenity",                  /* translation domain */
    "/usr/share/locale",         /* domain dir */
    "zh:ja:ko:*"                 /* default locales */
};

static const GtkIMContextInfo* info_list[] = { &serenity_info };

void im_module_list(const GtkIMContextInfo*** contexts, guint* n_contexts)
{
    *contexts = info_list;
    *n_contexts = G_N_ELEMENTS(info_list);
}

void im_module_init(GTypeModule* module)
{
    serenity_im_context_register_type(module);
}

void im_module_exit(void)
{
}

GtkIMContext* im_module_create(const gchar* context_id)
{
    if (context_id && strcmp(context_id, "serenity") == 0)
        return g_object_new(serenity_im_context_get_type(), NULL);
    return NULL;
}
