/* Copyright 2017 - 2019 Rob King <jking@deadpixi.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
   TODO: (feature requests)
     Enable cmdline flag to set tabstops
   BUGS:
     If you build 3 windows, then close one, ctrl-G o does
       not change windows.  (lastfocus gets lost)
     SIGWINCH is not handled at all
     scrollback seems to be bounded by 100 lines rather than 1000, and
       only works after you've split at least one new window.
 */

#include "main.h"

static struct handler keys[128];
static struct handler cmd_keys[128];
static struct handler code_keys[KEY_MAX - KEY_MIN + 1];
static struct handler (*binding)[128] = &keys;
static NODE *root, *focused, *lastfocused = NULL;
static char commandkey = CTL(COMMAND_KEY);
static int nfds = 1; /* stdin */
static fd_set fds;
static char iobuf[BUFSIZ];
int scrollback_history = 1024;

static void reshape(NODE *n, int y, int x, int h, int w);
static void draw(NODE *n);
static void reshapechildren(NODE *n);
static const char *term = NULL;
static void freenode(NODE *n, bool recursive);

void
safewrite(int fd, const char *b, size_t n) /* Write, retry on interrupt */
{
	ssize_t s;
	while( n > 0 && ((s = write(fd, b, n)) >= 0 || errno == EINTR)) {
		if( s > 0 ) {
			b += s;
			n -= s;
		}
	}
}

static const char *
getshell(void) /* Get the user's preferred shell. */
{
	const char *shell = getenv("SHELL");
	struct passwd *pwd = shell ? NULL : getpwuid(getuid());
	return shell ? shell : pwd ? pwd->pw_shell : "/bin/sh";
}

static bool *
newtabs(int w, int ow, bool *oldtabs) /* Initialize default tabstops. */
{
	bool *tabs = calloc(w, sizeof *tabs);
	for( int i = 0; tabs != NULL && i < w; i++ ) {
		tabs[i] = i < ow ? oldtabs[i] : i % 8 == 0;
	}
	return tabs;
}

static NODE *
newnode(Node t, NODE *p, int y, int x, int h, int w) /* Create a new node. */
{
	NODE *n = NULL;
	if( h > 1 && w > 1 && (n = calloc(1, sizeof *n)) != NULL ) {
		if( (n->tabs = newtabs(w, 0, NULL)) == NULL ) {
			free(n);
			n = NULL;
		} else {
			n->t = t;
			n->parent = p;
			n->y = y;
			n->x = x;
			n->h = h;
			n->ntabs = n->w = w;
			n->pt = -1;
		}
	}
	return n;
}

static void
freenode(NODE *n, bool recurse) /* Free a node. */
{
	if( n ) {
		if( lastfocused == n )
			lastfocused = NULL;
		if( n->pri.win )
			delwin(n->pri.win);
		if( n->alt.win )
			delwin(n->alt.win);
		if( recurse ) {
			freenode(n->c1, true);
			freenode(n->c2, true);
		}
		if( n->pt >= 0 ) {
			close(n->pt);
			FD_CLR(n->pt, &fds);
		}
		free(n->tabs);
		free(n);
	}
}

static void
fixcursor(void) /* Move the terminal cursor to the active view. */
{
    if (focused){
        int y, x;
        curs_set(focused->s->off != focused->s->tos? 0 : focused->s->vis);
        getyx(focused->s->win, y, x);
        y = MIN(MAX(y, focused->s->tos), focused->s->tos + focused->h - 1);
        wmove(focused->s->win, y, x);
    }
}

static const char *
getterm(void)
{
    const char *envterm = getenv("TERM");
    if (term)
        return term;
    if (envterm && COLORS >= 256 && !strstr(DEFAULT_TERMINAL, "-256color"))
        return DEFAULT_256_COLOR_TERMINAL;
    return DEFAULT_TERMINAL;
}

static NODE *
newview(NODE *p, int y, int x, int h, int w) /* Open a new view. */
{
    struct winsize ws = {.ws_row = h, .ws_col = w};
    NODE *n = newnode(VIEW, p, y, x, h, w);
    if (!n)
        return NULL;

    SCRN *pri = &n->pri, *alt = &n->alt;
    pri->win = newpad(MAX(h, scrollback_history), w);
    alt->win = newpad(h, w);
    if (!pri->win || !alt->win)
        return freenode(n, false), NULL;
    pri->tos = pri->off = MAX(0, scrollback_history - h);
    n->s = pri;

    nodelay(pri->win, TRUE); nodelay(alt->win, TRUE);
    scrollok(pri->win, TRUE); scrollok(alt->win, TRUE);
    keypad(pri->win, TRUE); keypad(alt->win, TRUE);

    setupevents(n);

    pid_t pid = forkpty(&n->pt, NULL, NULL, &ws);
    if (pid < 0){
        if (!p)
            perror("forkpty");
        return freenode(n, false), NULL;
    } else if (pid == 0){
        char buf[100] = {0};
        snprintf(buf, sizeof(buf) - 1, "%lu", (unsigned long)getppid());
        setsid();
        setenv("MTM", buf, 1);
        setenv("TERM", getterm(), 1);
        signal(SIGCHLD, SIG_DFL);
        execl(getshell(), getshell(), NULL);
        return NULL;
    }

    FD_SET(n->pt, &fds);
    fcntl(n->pt, F_SETFL, O_NONBLOCK);
    nfds = n->pt > nfds? n->pt : nfds;
    return n;
}

static NODE *
newcontainer(Node t, NODE *p, int y, int x, int h, int w,
             NODE *c1, NODE *c2) /* Create a new container */
{
    NODE *n = newnode(t, p, y, x, h, w);
    if (!n)
        return NULL;

    n->c1 = c1;
    n->c2 = c2;
    c1->parent = c2->parent = n;

    reshapechildren(n);
    return n;
}

static void
focus(NODE *n) /* Focus a node. */
{
    if (!n)
        return;
    else if (n->t == VIEW){
        lastfocused = focused;
        focused = n;
    } else
        focus(n->c1? n->c1 : n->c2);
}

#define ABOVE(n) n->y - 2, n->x + n->w / 2
#define BELOW(n) n->y + n->h + 2, n->x + n->w / 2
#define LEFT(n)  n->y + n->h / 2, n->x - 2
#define RIGHT(n) n->y + n->h / 2, n->x + n->w + 2

static NODE *
findnode(NODE *n, int y, int x) /* Find the node enclosing y,x. */
{
    #define IN(n, y, x) (y >= n->y && y <= n->y + n->h && \
                         x >= n->x && x <= n->x + n->w)
    if (IN(n, y, x)){
        if (n->c1 && IN(n->c1, y, x))
            return findnode(n->c1, y, x);
        if (n->c2 && IN(n->c2, y, x))
            return findnode(n->c2, y, x);
        return n;
    }
    return NULL;
}

static void
replacechild(NODE *n, NODE *c1, NODE *c2) /* Replace c1 of n with c2. */
{
    c2->parent = n;
    if (!n){
        root = c2;
        reshape(c2, 0, 0, LINES, COLS);
    } else if (n->c1 == c1)
        n->c1 = c2;
    else if (n->c2 == c1)
        n->c2 = c2;

    n = n? n : root;
    reshape(n, n->y, n->x, n->h, n->w);
    draw(n);
}

static void
removechild(NODE *p, const NODE *c) /* Replace p with other child. */
{
    replacechild(p->parent, p, c == p->c1? p->c2 : p->c1);
    freenode(p, false);
}

static int
deletenode(NODE *n, const char **args) /* Delete a node. */
{
	(void) args;
    if (!n || !n->parent)
        exit(EXIT_SUCCESS);
    if (n == focused)
        focus(n->parent->c1 == n? n->parent->c2 : n->parent->c1);
    removechild(n->parent, n);
    freenode(n, true);
	return 0;
}

static void
reshapeview(NODE *n, int d, int ow) /* Reshape a view. */
{
    int oy, ox;
    bool *tabs = newtabs(n->w, ow, n->tabs);
    struct winsize ws = {.ws_row = n->h, .ws_col = n->w};

    if (tabs){
        free(n->tabs);
        n->tabs = tabs;
        n->ntabs = n->w;
    }

    getyx(n->s->win, oy, ox);
    wresize(n->pri.win, MAX(n->h, scrollback_history), MAX(n->w, 2));
    wresize(n->alt.win, MAX(n->h, 2), MAX(n->w, 2));
    n->pri.tos = n->pri.off = MAX(0, scrollback_history - n->h);
    n->alt.tos = n->alt.off = 0;
    wsetscrreg(n->pri.win, 0, MAX(scrollback_history, n->h) - 1);
    wsetscrreg(n->alt.win, 0, n->h - 1);
    if (d > 0){ /* make sure the new top line syncs up after reshape */
        wmove(n->s->win, oy + d, ox);
        wscrl(n->s->win, -d);
    }
    doupdate();
    refresh();
    ioctl(n->pt, TIOCSWINSZ, &ws);
}

static void
reshapechildren(NODE *n) /* Reshape all children of a view. */
{
    if (n->t == HORIZONTAL){
        int i = n->w % 2? 0 : 1;
        reshape(n->c1, n->y, n->x, n->h, n->w / 2);
        reshape(n->c2, n->y, n->x + n->w / 2 + 1, n->h, n->w / 2 - i);
    } else if (n->t == VERTICAL){
        int i = n->h % 2? 0 : 1;
        reshape(n->c1, n->y, n->x, n->h / 2, n->w);
        reshape(n->c2, n->y + n->h / 2 + 1, n->x, n->h / 2 - i, n->w);
    }
}

static void
reshape(NODE *n, int y, int x, int h, int w) /* Reshape a node. */
{
    if (n->y == y && n->x == x && n->h == h && n->w == w && n->t == VIEW)
        return;

    int d = n->h - h;
    int ow = n->w;
    n->y = y;
    n->x = x;
    n->h = MAX(h, 1);
    n->w = MAX(w, 1);

    if (n->t == VIEW)
        reshapeview(n, d, ow);
    else
        reshapechildren(n);
    draw(n);
}

static void
drawchildren(const NODE *n) /* Draw all children of n. */
{
	draw(n->c1);
	if( binding == &cmd_keys ) {
		attron(A_REVERSE);
	}
	if (n->t == HORIZONTAL)
		mvvline(n->y, n->x + n->w / 2, ACS_VLINE, n->h);
	else
		mvhline(n->y + n->h / 2, n->x, ACS_HLINE, n->w);
	if( binding == &cmd_keys ) {
		attroff(A_REVERSE);
	}
	wnoutrefresh(stdscr);
	draw(n->c2);
}

static void
draw(NODE *n) /* Draw a node. */
{
	if( n->t == VIEW ) {
		pnoutrefresh(
			n->s->win,  /* pad */
			n->s->off,  /* pminrow */
			0,          /* pmincol */
			n->y,       /* sminrow */
			n->x,       /* smincol */
			n->y + n->h - 1,  /* smaxrow */
			n->x + n->w - 1   /* smaxcol */
		);
	} else {
		drawchildren(n);
	}
}

static int
split(NODE *n, const char *args[])
{
	Node t = args[0] && args[0][0] == 'v' ? HORIZONTAL : VERTICAL;

    int nh = t == VERTICAL? (n->h - 1) / 2 : n->h;
    int nw = t == HORIZONTAL? (n->w) / 2 : n->w;
    NODE *p = n->parent;
    NODE *v = newview(NULL, 0, 0, MAX(0, nh), MAX(0, nw));
    if (!v)
        return -1;

    NODE *c = newcontainer(t, n->parent, n->y, n->x, n->h, n->w, n, v);
    if (!c){
        freenode(v, false);
        return -1;
    }

    replacechild(p, n, c);
    focus(v);
    draw(p? p : root);
	return 0;
}

static bool
getinput(NODE *n, fd_set *f) /* Recursively check all ptty's for input. */
{
    if (n && n->c1 && !getinput(n->c1, f))
        return false;

    if (n && n->c2 && !getinput(n->c2, f))
        return false;

    if (n && n->t == VIEW && n->pt > 0 && FD_ISSET(n->pt, f)){
        ssize_t r = read(n->pt, iobuf, sizeof(iobuf));
        if (r > 0)
            vtwrite(&n->vp, iobuf, r);
        if (r <= 0 && errno != EINTR && errno != EWOULDBLOCK)
            return deletenode(n, NULL), false;
    }

    return true;
}

static void
scrollbottom(NODE *n)
{
    n->s->off = n->s->tos;
}

static int
scrolln(NODE *n, const char **args)
{
	if(args[0][0] == '-') {
		n->s->off = MAX(0, n->s->off - n->h / 2);
	} else {
		n->s->off = MIN(n->s->tos, n->s->off + n->h / 2);
	}
	return 0;
}

static int
sendarrow(NODE *n, const char **args)
{
	const char *k = args[0];
    char buf[100] = {0};
    snprintf(buf, sizeof(buf) - 1, "\033%s%s", n->pnm? "O" : "[", k);
    safewrite(n->pt, buf, strlen(buf));
    return 0;
}

int
reshape_root(NODE *n, const char **args)
{
	(void)args;
	reshape(root, 0, 0, LINES, COLS);
	scrollbottom(n);
	return 0;
}

int
sendnewline(NODE *n, const char **args)
{
	(void)args;
	char *nl = n->lnm ? "\r\n" : "\r";
	int len = n->lnm ? 2 : 1;
	safewrite(n->pt, nl, len);
	scrollbottom(n);
	return 0;
}

int
mov(struct NODE *n, const char **args)
{
	switch(args[0][0]) {
	case 'u':
		focus(findnode(root, ABOVE(n)));
		break;
	case 'd':
		focus(findnode(root, BELOW(n)));
		break;
	case 'l':
		focus(findnode(root, LEFT(n)));
		break;
	case 'r':
		focus(findnode(root, RIGHT(n)));
		break;
	case 'p':
		focus(lastfocused);
		break;
	default:
		assert(0);
	}
	return 0;
}

static int
redrawroot(struct NODE *n, const char **args)
{
	(void) n;
	(void) args;
	touchwin(stdscr);
	draw(root);
	redrawwin(stdscr);
	return 0;
}

int
send(NODE *n, const char **args)
{
	size_t len = args[1] ? strtoul(args[1], NULL, 10 ) : strlen(args[0]);
	safewrite(n->pt, args[0], len);
	scrollbottom(n);
	return 0;
}

int
transition(NODE *n, const char **args)
{
	assert(args);
	binding = binding == &keys ? &cmd_keys : &keys;
	if( args[0] ) {
		send(n, args);
	}
	if( binding == &keys ) {
		scrollbottom(n);
	}
	return 0;
}

static void
add_key(struct handler *b, wchar_t k, action act, ...)
{
	if( b == code_keys ) {
		assert( k >= KEY_MIN && k <= KEY_MAX );
		k -= KEY_MIN;
	}
	int i = 0;
	b[k].act = act;
	va_list ap;
	va_start(ap, act);
	do b[k].args[i] = va_arg(ap, const char *);
	while( b[k].args[i++] != NULL );
	va_end(ap);
}

static void
build_bindings()
{
	assert( KEY_MAX - KEY_MIN < 2048 ); /* Avoid overly large luts */

	add_key(keys, commandkey, transition, NULL);
	add_key(keys, L'\r', sendnewline, NULL);
	add_key(keys, L'\n', send, "\n", NULL);
	add_key(keys, 0, send, "\000", "1", NULL);

	add_key(cmd_keys, commandkey, transition, &commandkey, "1", NULL);
	add_key(cmd_keys, L'\r', transition, NULL);
	add_key(cmd_keys, L',', scrolln, "-1", NULL);
	add_key(cmd_keys, L'm', scrolln, "+1", NULL);
	add_key(cmd_keys, L'c', split, NULL);
	add_key(cmd_keys, L'|', split, "v", NULL);
	add_key(cmd_keys, L'w', deletenode, NULL);
	add_key(cmd_keys, L'r', redrawroot, NULL);
	add_key(cmd_keys, L'j', mov, "down", NULL);
	add_key(cmd_keys, L'k', mov, "up", NULL);
	add_key(cmd_keys, L'h', mov, "left", NULL);
	add_key(cmd_keys, L'l', mov, "right", NULL);
	add_key(cmd_keys, L'o', mov, "previous", NULL);

	add_key(code_keys, KEY_RESIZE, reshape_root, NULL);
	add_key(code_keys, KEY_F(1), send, "\033OP", NULL);
	add_key(code_keys, KEY_F(2), send, "\033OQ", NULL);
	add_key(code_keys, KEY_F(3), send, "\033OR", NULL);
	add_key(code_keys, KEY_F(4), send, "\033OS", NULL);
	add_key(code_keys, KEY_F(5), send, "\033[15~", NULL);
	add_key(code_keys, KEY_F(6), send, "\033[17~", NULL);
	add_key(code_keys, KEY_F(7), send, "\033[18~", NULL);
	add_key(code_keys, KEY_F(8), send, "\033[19~", NULL);
	add_key(code_keys, KEY_F(9), send, "\033[20~", NULL);
	add_key(code_keys, KEY_F(10), send, "\033[21~", NULL);
	add_key(code_keys, KEY_F(11), send, "\033[23~", NULL);
	add_key(code_keys, KEY_F(12), send, "\033[24~", NULL);
	add_key(code_keys, KEY_HOME, send, "\033[1~", NULL);
	add_key(code_keys, KEY_END, send, "\033[4~", NULL);
	add_key(code_keys, KEY_PPAGE, send, "\033[5~", NULL);
	add_key(code_keys, KEY_NPAGE, send, "\033[6~", NULL);
	add_key(code_keys, KEY_BACKSPACE, send, "\177", NULL);
	add_key(code_keys, KEY_DC, send, "\033[3~", NULL);
	add_key(code_keys, KEY_IC, send, "\033[2~", NULL);
	add_key(code_keys, KEY_BTAB, send, "\033[Z", NULL);
	add_key(code_keys, KEY_ENTER, sendnewline, NULL);
	add_key(code_keys, KEY_UP, sendarrow, "A", NULL);
	add_key(code_keys, KEY_DOWN, sendarrow, "B", NULL);
	add_key(code_keys, KEY_RIGHT, sendarrow, "C", NULL);
	add_key(code_keys, KEY_LEFT, sendarrow, "D", NULL);
}

static bool
handlechar(int r, int k) /* Handle a single input character. */
{
	struct handler *b = NULL;
	int rv = 0;
	NODE *n = focused;

	if( r == OK && k > 0 && k < (int)sizeof *binding ) {
		b = &(*binding)[k];
	} else if( r == KEY_CODE_YES ) {
		assert( k >= KEY_MIN && k <= KEY_MAX );
		b = &code_keys[k - KEY_MIN];
	}

	if( b && b->act ) {
		rv = b->act(n, b->args);
	} else {
		char c[MB_LEN_MAX + 1] = {0};
		if( r != ERR && wctomb(c, k) > 0 ) {
			scrollbottom(n);
			safewrite(n->pt, c, strlen(c));
		}
	}
	return r != ERR;
}

static void
run(void)
{
	while( root != NULL ) {
		int r;
		wint_t w = 0;
		fd_set sfds = fds;
		if( select(nfds + 1, &sfds, NULL, NULL, NULL) < 0 ) {
			FD_ZERO(&sfds);
		}
		do {
			r = wget_wch(focused->s->win, &w);
		} while( handlechar(r, w) );
		getinput(root, &sfds);
		draw(root);
		doupdate();
		fixcursor();
		draw(focused);
		doupdate();
	}
}

static void
parse_args(int argc, char **argv)
{
	int c;
	char *name = strrchr(argv[0], '/');
	while( (c = getopt(argc, argv, ":hc:s:T:t:")) != -1 ) {
		switch (c) {
		case 'h':
			printf("usage: %s [-T NAME] [-t NAME] [-c KEY]\n",
				name ? name + 1 : argv[0]);
			exit(0);
		case 'c':
			commandkey = CTL(optarg[0]);
			break;
		case 's':
			scrollback_history = strtol(optarg, NULL, 10);
			break;
		case 'T':
			setenv("TERM", optarg, 1);
			break;
		case 't':
			term = optarg;
			break;
		default:
			fprintf(stderr, "Unkown option: %c\n", optopt);
			exit(EXIT_FAILURE);
		}
	}
}


void
cleanup(void)
{
	if (root) {
		freenode(root, true);
	}
	endwin();
}

int
main(int argc, char **argv)
{
	FD_SET(STDIN_FILENO, &fds);
	setlocale(LC_ALL, "");
	signal(SIGCHLD, SIG_IGN); /* automatically reap children */
	parse_args(argc, argv);
	build_bindings();

	if( initscr() == NULL ) {
		exit(EXIT_FAILURE);
	}
	atexit(cleanup);
	raw();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	start_color();
	use_default_colors();

	root = newview(NULL, 0, 0, LINES, COLS);
	if( root == NULL ) {
		err(EXIT_FAILURE, "Unable to create root window");
	}
	focus(root);
	draw(root);
	run();
	return EXIT_SUCCESS;
}
