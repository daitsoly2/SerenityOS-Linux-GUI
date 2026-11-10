/* Internal hook, not part of the libadwaita public API. */
#ifndef ADW_SERENITY_H
#define ADW_SERENITY_H

#include <glib.h>

/* SERENITY_HOOK: every symbol introduced by this fork is hidden.
   The exported symbol tables of libgtk-3.so.0 / libgtk-4.so.1 /
   libadwaita-1.so.0 stay byte-for-byte identical to upstream. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

G_BEGIN_DECLS

void adw_serenity_init (void);

G_END_DECLS

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
#endif
