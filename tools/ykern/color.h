#ifndef YKERN_TOOLS_COLOR_H
#define YKERN_TOOLS_COLOR_H

/*
 * Tiny ANSI-color helpers for the ykern CLI.
 *
 * Every function returns a `const char *` — either the proper ANSI sequence
 * when stdout is a TTY (and NO_COLOR isn't set), or an empty string when
 * it isn't. That means callers can interpolate them into format strings
 * unconditionally:
 *
 *     printf("%s[%s]%s %s%s%s\n", col_kind(), kind, col_reset(),
 *            col_path(), path, col_reset());
 *
 * No `if (use_color) ...` branches sprinkled around; no escape codes
 * literal in printf format strings.
 *
 * Override the autodetection with environment variables:
 *   NO_COLOR=<anything>     force off (https://no-color.org/)
 *   CLICOLOR_FORCE=1        force on (e.g. when piping into less -R)
 */

void color_init(void);

/* Reset / styling primitives. */
const char *col_reset(void);
const char *col_bold(void);
const char *col_dim(void);

/* Plain colours. */
const char *col_red(void);
const char *col_green(void);
const char *col_yellow(void);
const char *col_blue(void);
const char *col_magenta(void);
const char *col_cyan(void);

/* Semantic shortcuts used by the CLI's print routines. */
const char *col_kind(void);     /* "[operation]" tag */
const char *col_path(void);     /* the path after the tag */
const char *col_section(void);  /* "Invoke:", "What you can do here:" headers */
const char *col_bullet(void);   /* the leading "•" */
const char *col_command(void);  /* `ykern ...` example commands */
const char *col_attr(void);     /* attribute names in listings */
const char *col_error(void);    /* error / errno surface */

#endif /* YKERN_TOOLS_COLOR_H */
