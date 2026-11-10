/* Dump the active Serenity SystemTheme as seen by the patched GTK libraries.
 * Usage: serenity-theme-dump [--css 3|4] [--json] */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "serenity-ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *role_names[SERENITY_ROLE_COUNT] = {
    "face", "face_text", "window", "window_text", "base", "base_text",
    "highlight", "light", "shadow", "selection", "selection_text",
    "menu", "menu_text", "menu_selection", "menu_selection_text",
    "menu_stripe", "disabled_text", "disabled_text_back", "hover",
    "title_active_1", "title_active_2", "title_text",
    "title_inactive_1", "title_inactive_2", "title_inactive_text",
    "active_border", "inactive_border", "gutter", "tooltip", "tooltip_text",
    "link", "placeholder", "focus_outline",
};

int main(int argc, char **argv)
{
    int i, css = 0, json = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--css") == 0 && i + 1 < argc) css = atoi(argv[++i]);
        else if (strcmp(argv[i], "--json") == 0) json = 1;
    }
    if (css) {
        fputs(serenity_ui_css(css), stdout);
        return 0;
    }
    if (json) {
        printf("{\n  \"theme\": \"%s\",\n  \"path\": \"%s\",\n",
               serenity_ui_theme_name(), serenity_ui_theme_path());
        printf("  \"font\": \"%s\",\n  \"font_size\": %d,\n",
               serenity_ui_font_family(), serenity_ui_font_size());
        printf("  \"revision\": %llu,\n  \"colors\": {\n",
               (unsigned long long)serenity_ui_revision());
        for (i = 0; i < SERENITY_ROLE_COUNT; i++) {
            char h[8];
            serenity_ui_color_hex((SerenityRole)i, h);
            printf("    \"%s\": \"%s\"%s\n", role_names[i], h,
                   i + 1 < SERENITY_ROLE_COUNT ? "," : "");
        }
        printf("  }\n}\n");
        return 0;
    }
    printf("theme      : %s\n", serenity_ui_theme_name());
    printf("source     : %s\n", serenity_ui_theme_path());
    printf("font       : %s %dpt\n", serenity_ui_font_family(), serenity_ui_font_size());
    printf("revision   : %llu\n", (unsigned long long)serenity_ui_revision());
    printf("geometry   : bevel=%d border=%d radius=%d focus=%d button_h=%d scrollbar=%d check=%d titlebar=%d\n",
           serenity_ui_metric(SERENITY_METRIC_BEVEL),
           serenity_ui_metric(SERENITY_METRIC_BORDER),
           serenity_ui_metric(SERENITY_METRIC_RADIUS),
           serenity_ui_metric(SERENITY_METRIC_FOCUS_MARGIN),
           serenity_ui_metric(SERENITY_METRIC_BUTTON_MIN_H),
           serenity_ui_metric(SERENITY_METRIC_SCROLLBAR_W),
           serenity_ui_metric(SERENITY_METRIC_CHECK_SIZE),
           serenity_ui_metric(SERENITY_METRIC_TITLEBAR_H));
    for (i = 0; i < SERENITY_ROLE_COUNT; i++) {
        char h[8];
        serenity_ui_color_hex((SerenityRole)i, h);
        printf("  %-24s %s\n", role_names[i], h);
    }
    return 0;
}
