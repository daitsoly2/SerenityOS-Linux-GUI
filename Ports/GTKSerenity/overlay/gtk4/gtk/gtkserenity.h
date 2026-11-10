/* Internal Serenity widget-rendering bridge for GTK4.
 * Compiled into libgtk-4.so.1 with hidden visibility; exports no symbols. */
#ifndef __GTK_SERENITY_H__
#define __GTK_SERENITY_H__

#include <gtk/gtkwidget.h>
#include <gtk/gtksnapshot.h>
#include <graphene.h>

/* SERENITY_HOOK: every symbol introduced by this fork is hidden.
   The exported symbol tables of libgtk-3.so.0 / libgtk-4.so.1 /
   libadwaita-1.so.0 stay byte-for-byte identical to upstream. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

G_BEGIN_DECLS

/* Bracket gtk_widget_do_snapshot(): paints the Serenity widget background
 * into the render node tree before the widget's own children are drawn. */
void     _gtk_serenity_snapshot_begin (GtkWidget *widget, GtkSnapshot *snapshot);
void     _gtk_serenity_snapshot_end   (GtkWidget *widget);

/* Queried by gtk_css_style_snapshot_background()/border(): when TRUE the
 * stock Adwaita CSS drawing is skipped because we already painted it. */
gboolean _gtk_serenity_suppress_background (void);
gboolean _gtk_serenity_suppress_border     (void);

/* SERENITY_HOOK: called from gtk_widget_create_render_node(), the single
 * point every widget (root and children) passes, with the exact border-box
 * geometry. Paints the widget's background/border the way Serenity's
 * ClassicStylePainter would. Returns TRUE when it painted, so the caller
 * skips the stock CSS background/border. This is the "forward the paint
 * request to Serenity" entry point. */
gboolean _gtk_serenity_paint (GtkWidget *widget,
                              GtkSnapshot *snapshot,
                              const graphene_rect_t *box);

void     _gtk_serenity_init                (GdkDisplay *display);

G_END_DECLS

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
#endif /* __GTK_SERENITY_H__ */
