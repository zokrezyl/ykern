#include "color.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_use_color = 0;

void color_init(void)
{
    /* NO_COLOR wins regardless of TTY status (the convention is that any
     * non-empty value disables colour). */
    const char *no_color = getenv("NO_COLOR");
    if (no_color && *no_color) {
        g_use_color = 0;
        return;
    }
    /* CLICOLOR_FORCE=1 turns colour on even when piping. */
    const char *force = getenv("CLICOLOR_FORCE");
    if (force && *force && strcmp(force, "0") != 0) {
        g_use_color = 1;
        return;
    }
    g_use_color = isatty(STDOUT_FILENO) ? 1 : 0;
}

#define IF_COLOR(seq) (g_use_color ? (seq) : "")

const char *col_reset(void) { return IF_COLOR("\033[0m"); }
const char *col_bold(void)  { return IF_COLOR("\033[1m"); }
const char *col_dim(void)   { return IF_COLOR("\033[2m"); }

const char *col_red(void)     { return IF_COLOR("\033[31m"); }
const char *col_green(void)   { return IF_COLOR("\033[32m"); }
const char *col_yellow(void)  { return IF_COLOR("\033[33m"); }
const char *col_blue(void)    { return IF_COLOR("\033[34m"); }
const char *col_magenta(void) { return IF_COLOR("\033[35m"); }
const char *col_cyan(void)    { return IF_COLOR("\033[36m"); }

/* Semantic shortcuts. Tweaked here so the whole tool restyles in one place. */
const char *col_kind(void)    { return IF_COLOR("\033[1;36m"); } /* bold cyan */
const char *col_path(void)    { return IF_COLOR("\033[1m");     } /* bold */
const char *col_section(void) { return IF_COLOR("\033[1;32m"); } /* bold green */
const char *col_bullet(void)  { return IF_COLOR("\033[32m");   } /* green */
const char *col_command(void) { return IF_COLOR("\033[36m");   } /* cyan */
const char *col_attr(void)    { return IF_COLOR("\033[33m");   } /* yellow */
const char *col_error(void)   { return IF_COLOR("\033[1;31m"); } /* bold red */
