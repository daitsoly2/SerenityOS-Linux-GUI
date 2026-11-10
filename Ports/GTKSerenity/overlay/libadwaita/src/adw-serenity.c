/* libadwaita -> Serenity policy hook.
 *
 * All libadwaita widgets are GTK4 widgets, so their *painting* is already
 * replaced by the GTK4 Serenity bridge. What remains here is policy:
 *   1. no dark variant / no portal.Settings probing (also removes the
 *      25-second startup stall on a desktop without xdg-desktop-portal);
 *   2. no animations — the classic desktop has no transitions;
 *   3. force one colour scheme so AdwStyleManager cannot reintroduce
 *      Adwaita's own palette behind our widgets.
 */
#include "config.h"
#include "adw-serenity.h"

#include "adw-style-manager.h"
#include "serenity-ui.h"

#include <gtk/gtk.h>

void
adw_serenity_init (void)
{
  AdwStyleManager *manager;
  GtkSettings *settings;

  if (!serenity_ui_is_active ())
    return;

  manager = adw_style_manager_get_default ();
  if (manager)
    adw_style_manager_set_color_scheme (manager, ADW_COLOR_SCHEME_FORCE_LIGHT);

  settings = gtk_settings_get_default ();
  if (settings)
    g_object_set (settings, "gtk-enable-animations", FALSE, NULL);
}
