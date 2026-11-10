/* Internal Serenity widget-rendering bridge for GTK3.
 * Not installed, not exported: all symbols are hidden by the GTK build. */
#ifndef __GTK_SERENITY_H__
#define __GTK_SERENITY_H__

#include <cairo.h>
#include <gdk/gdk.h>
#include <gtk/gtkstylecontext.h>
#include <gtk/gtkwidget.h>

/* SERENITY_HOOK: every symbol introduced by this fork is hidden.
   The exported symbol tables of libgtk-3.so.0 / libgtk-4.so.1 /
   libadwaita-1.so.0 stay byte-for-byte identical to upstream. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

G_BEGIN_DECLS

/* Return TRUE when the shim painted the widget, meaning the stock GTK
 * CSS renderer must be skipped for this call. */
gboolean _gtk_serenity_render_background (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);
gboolean _gtk_serenity_render_frame      (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);
gboolean _gtk_serenity_render_check      (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);
gboolean _gtk_serenity_render_option     (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);
gboolean _gtk_serenity_render_slider     (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height,
                                          GtkOrientation orientation);
gboolean _gtk_serenity_render_handle     (GtkStyleContext *context,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);
gboolean _gtk_serenity_render_arrow      (GtkStyleContext *context,
                                          cairo_t *cr,
                                          gdouble angle, gdouble x, gdouble y,
                                          gdouble size);

/* Gadget-path hook: gadgets (GtkMenuItem, GtkModelButton, GtkButton, ...)
 * do not go through gtk_render_background(), they paint straight from
 * gtk_css_gadget_draw(). Called instead of the stock CSS background/border
 * render; returns TRUE when the stock paint must be skipped. */
gboolean _gtk_serenity_render_gadget     (GtkWidget *owner,
                                          const gchar *node_name,
                                          GtkStateFlags state,
                                          cairo_t *cr, gdouble x, gdouble y,
                                          gdouble width, gdouble height);

/* Called once per process; installs the generated Serenity stylesheet at a
 * priority above application CSS and starts the theme-revision watcher. */
void     _gtk_serenity_init              (GdkScreen *screen);

G_END_DECLS

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
#endif /* __GTK_SERENITY_H__ */
