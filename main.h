#include "config.h"
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "vtparser.h"
/* mtm by default will advertise itself as a "screen-bce" terminal.
 * This is the terminal type advertised by such programs as
 * screen(1) and tmux(1) and is a widely-supported terminal type.
 * mtm supports emulating the "screen-bce" terminal very well, and this
 * is a reasonable default.
 *
 * However, you can change the default terminal that mtm will
 * advertise itself as. There's the "mtm" terminal type that is
 * recommended for use if you know it will be available in all the
 * environments in which mtm will be used. It advertises a few
 * features that mtm has that the default "screen-bce" terminfo doesn't
 * list, meaning that terminfo-aware programs may get a small
 * speed boost.
 */
#define DEFAULT_TERMINAL "screen-bce"
#define DEFAULT_256_COLOR_TERMINAL "screen-256color-bce"

/* The default command prefix key, when modified by cntrl.
 * This can be changed at runtime using the '-c' flag.
 */
#define COMMAND_KEY 'g'


#if HAVE_CURSES_H
# include <curses.h>
#elif HAVE_NCURSESW_CURSES_H
# include <ncursesw/curses.h>
#endif
#if HAVE_PTY_H
# include <pty.h>
#elif HAVE_LIBUTIL_H
# include <libutil.h>
#elif HAVE_UTIL_H
# include <util.h>
#endif

#define MIN(x, y) ((x) < (y)? (x) : (y))
#define MAX(x, y) ((x) > (y)? (x) : (y))
#define CTL(x) ((x) & 0x1f)

struct screen {
	int sy, sx, vis, tos, off;
	short fg, bg, sfg, sbg, sp;
	bool insert, oxenl, xenl, saved;
	attr_t sattr;
	WINDOW *win;
};

typedef struct node NODE;
struct node {
	int t;  /* '|', '-', or '\0' (lateral, transverse, or no split) */
	int y, x, h, w, pt, ntabs;
	bool *tabs, pnm, decom, am, lnm;
	wchar_t repc;
	NODE *parent;
	NODE *c1, *c2;
	struct screen pri, alt, *s;
	wchar_t *g0, *g1, *g2, *g3, *gc, *gs, *sgc, *sgs;
	VTPARSER vp;
};

typedef int(action)(NODE *n, const char **args);
struct handler {
	action *act;
	const char *args[7];
};

#define MAXMAP 0x7f
extern wchar_t CSET_US[]; /* "USASCII" */
extern wchar_t CSET_UK[]; /* "United Kingdom" */
extern wchar_t CSET_GRAPH[]; /* Graphics Set One */
extern int scrollback_history;
extern int tabstop;

void setupevents(NODE *n);
void safewrite(int fd, const char *b, size_t n);
