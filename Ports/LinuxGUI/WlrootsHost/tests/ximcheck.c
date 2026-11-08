/* 极简 XIM 客户端：只验证 XOpenIM/XCreateIC 能否连上 @server=serenity。 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xlocale.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    setlocale(LC_ALL, "");
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    char* mods = XSetLocaleModifiers("@im=serenity");
    fprintf(stderr, "XSetLocaleModifiers -> %s\n", mods ? mods : "(null)");
    XIM im = XOpenIM(d, NULL, NULL, NULL);
    fprintf(stderr, "XOpenIM -> %p\n", (void*)im);
    if (!im) return 2;
    XIMStyles* styles = NULL;
    XGetIMValues(im, XNQueryInputStyle, &styles, NULL);
    fprintf(stderr, "styles=%p count=%u\n", (void*)styles,
        styles ? (unsigned)styles->count_styles : 0u);
    XIC ic = XCreateIC(im, XNInputStyle,
        XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
        DefaultRootWindow(d), NULL);
    fprintf(stderr, "XCreateIC -> %p\n", (void*)ic);
    if (ic) XDestroyIC(ic);
    XCloseIM(im);
    XCloseDisplay(d);
    return 0;
}
